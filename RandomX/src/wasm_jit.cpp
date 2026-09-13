#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include "bytecode_machine.hpp"
#include "reciprocal.h"

namespace randomx {
namespace {

static void uleb(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t b = static_cast<uint8_t>(value & 0x7fu);
        value >>= 7;
        if (value) b |= 0x80u;
        out.push_back(b);
    } while (value);
}

static void sleb(std::vector<uint8_t>& out, int64_t value) {
    bool more = true;
    while (more) {
        uint8_t b = static_cast<uint8_t>(value & 0x7f);
        const bool sign = (b & 0x40u) != 0;
        value >>= 7;
        more = !((value == 0 && !sign) || (value == -1 && sign));
        if (more) b |= 0x80u;
        out.push_back(b);
    }
}

static void bytes(std::vector<uint8_t>& out, const char* s, size_t n) {
    uleb(out, static_cast<uint32_t>(n));
    out.insert(out.end(), s, s + n);
}

static void section(std::vector<uint8_t>& module, uint8_t id,
                    const std::vector<uint8_t>& payload) {
    module.push_back(id);
    uleb(module, static_cast<uint32_t>(payload.size()));
    module.insert(module.end(), payload.begin(), payload.end());
}

static void local_get(std::vector<uint8_t>& c, uint32_t index) {
    c.push_back(0x20);
    uleb(c, index);
}

static void local_set(std::vector<uint8_t>& c, uint32_t index) {
    c.push_back(0x21);
    uleb(c, index);
}

static void i32_const(std::vector<uint8_t>& c, int32_t value) {
    c.push_back(0x41);
    sleb(c, value);
}

static void i64_const(std::vector<uint8_t>& c, int64_t value) {
    c.push_back(0x42);
    sleb(c, value);
}

static void i64_load(std::vector<uint8_t>& c) {
    c.push_back(0x29);
    uleb(c, 3);
    uleb(c, 0);
}

static void i64_store(std::vector<uint8_t>& c) {
    c.push_back(0x37);
    uleb(c, 3);
    uleb(c, 0);
}

static void v128_load(std::vector<uint8_t>& c) {
    c.push_back(0xfd);
    uleb(c, 0x00);
    uleb(c, 4);
    uleb(c, 0);
}

static void v128_store(std::vector<uint8_t>& c) {
    c.push_back(0xfd);
    uleb(c, 0x0b);
    uleb(c, 4);
    uleb(c, 0);
}

static void v128_op(std::vector<uint8_t>& c, uint32_t op) {
    c.push_back(0xfd);
    uleb(c, op);
}

static void load_reg(std::vector<uint8_t>& c, uint32_t r) {
    local_get(c, 0);
    i32_const(c, static_cast<int32_t>(r * 8));
    c.push_back(0x6a);
    i64_load(c);
}

static void store_reg(std::vector<uint8_t>& c, uint32_t r) {
    local_get(c, 0);
    i32_const(c, static_cast<int32_t>(r * 8));
    c.push_back(0x6a);
    i64_store(c);
}

static void load_fp(std::vector<uint8_t>& c, uint32_t ptr_local,
                    uint32_t index) {
    local_get(c, ptr_local);
    i32_const(c, static_cast<int32_t>(index * 16));
    c.push_back(0x6a);
    v128_load(c);
}

static void store_fp(std::vector<uint8_t>& c, uint32_t ptr_local,
                     uint32_t index) {
    local_get(c, ptr_local);
    i32_const(c, static_cast<int32_t>(index * 16));
    c.push_back(0x6a);
    v128_store(c);
}

static void emit_address(std::vector<uint8_t>& c, uint32_t src,
                         int64_t imm, uint32_t mask, bool zero_register) {
    local_get(c, 4);
    if (zero_register) {
        i64_const(c, imm);
    } else {
        local_get(c, 5 + src);
        i64_const(c, imm);
        c.push_back(0x7c); // i64.add
    }
    i64_const(c, static_cast<int64_t>(mask));
    c.push_back(0x83); // i64.and
    c.push_back(0xa7); // i32.wrap_i64
    c.push_back(0x6a); // i32.add
}

static void emit_load(std::vector<uint8_t>& c, uint32_t src,
                      int64_t imm, uint32_t mask, bool zero_register) {
    emit_address(c, src, imm, mask, zero_register);
    i64_load(c);
}

static int reg(uint8_t r) {
    return static_cast<int>(r % RegistersCount);
}

static int flt(uint8_t r) {
    return static_cast<int>(r % RegisterCountFlt);
}

static bool supported(const Instruction& ins) {
    const int op = ins.opcode;
    if (op < ceil_FSWAP_R) return true;
    if (op < ceil_FADD_R) return true;
    if (op < ceil_FADD_M) return false;
    if (op < ceil_FSUB_R) return true;
    if (op < ceil_FSUB_M) return false;
    if (op < ceil_FSCAL_R) return true;
    if (op < ceil_FMUL_R) return false;
    if (op < ceil_FDIV_M) return true;
    if (op < ceil_FSQRT_R) return false;
    if (op < ceil_CBRANCH) return true;
    if (op < ceil_ISTORE) return false;
    if (op < ceil_NOP) return true;
    return true;
}

static void emit_i64_mulh_import(std::vector<uint8_t>& c, uint32_t fn) {
    c.push_back(0x10);
    uleb(c, fn);
}

} // namespace

bool WasmJit::compile(const Program& program) {
    module_.clear();

    for (uint32_t pc = 0; pc < program.getSize(); ++pc) {
        if (!supported(program(static_cast<int>(pc)))) return false;
    }

    std::vector<uint8_t> code;

    for (int r = 0; r < RegistersCount; ++r) {
        load_reg(code, static_cast<uint32_t>(r));
        local_set(code, static_cast<uint32_t>(5 + r));
    }
    for (int r = 0; r < RegisterCountFlt; ++r) {
        load_fp(code, 1, static_cast<uint32_t>(r));
        local_set(code, static_cast<uint32_t>(13 + r));
        load_fp(code, 2, static_cast<uint32_t>(r));
        local_set(code, static_cast<uint32_t>(17 + r));
        load_fp(code, 3, static_cast<uint32_t>(r));
        local_set(code, static_cast<uint32_t>(21 + r));
    }

    for (uint32_t pc = 0; pc < program.getSize(); ++pc) {
        const Instruction& ins = program(static_cast<int>(pc));
        const int op = ins.opcode;
        const int dst = reg(ins.dst);
        const int src = reg(ins.src);
        const int fdst = flt(ins.dst);
        const int fsrc = flt(ins.src);
        const int64_t simm = static_cast<int64_t>(static_cast<int32_t>(ins.getImm32()));

        if (op < ceil_IADD_RS) {
            local_get(code, 5 + dst);
            local_get(code, 5 + src);
            i64_const(code, static_cast<int64_t>(ins.getModShift()));
            code.push_back(0x86);
            if (dst == RegisterNeedsDisplacement) {
                i64_const(code, simm);
                code.push_back(0x7c);
            }
            code.push_back(0x7c);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IADD_M) {
            local_get(code, 5 + dst);
            const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero);
            code.push_back(0x7c);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_ISUB_R) {
            local_get(code, 5 + dst);
            if (src == dst) i64_const(code, simm);
            else local_get(code, 5 + src);
            code.push_back(0x7d);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_ISUB_M) {
            local_get(code, 5 + dst);
            const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero);
            code.push_back(0x7d);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IMUL_R) {
            local_get(code, 5 + dst);
            if (src == dst) i64_const(code, simm);
            else local_get(code, 5 + src);
            code.push_back(0x7e);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IMUL_M) {
            local_get(code, 5 + dst);
            const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero);
            code.push_back(0x7e);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IMULH_R) {
            local_get(code, 5 + dst);
            local_get(code, 5 + src);
            emit_i64_mulh_import(code, 0);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IMULH_M) {
            local_get(code, 5 + dst);
            const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero);
            emit_i64_mulh_import(code, 0);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_ISMULH_R) {
            local_get(code, 5 + dst);
            local_get(code, 5 + src);
            emit_i64_mulh_import(code, 1);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_ISMULH_M) {
            local_get(code, 5 + dst);
            const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero);
            emit_i64_mulh_import(code, 1);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IMUL_RCP) {
            const uint32_t divisor = ins.getImm32();
            if (!isZeroOrPowerOf2(divisor)) {
                local_get(code, 5 + dst);
                i64_const(code, static_cast<int64_t>(randomx_reciprocal(divisor)));
                code.push_back(0x7e);
                local_set(code, 5 + dst);
            }
        }
        else if (op < ceil_INEG_R) {
            local_get(code, 5 + dst);
            i64_const(code, 0);
            code.push_back(0x7d);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IXOR_R) {
            local_get(code, 5 + dst);
            if (src == dst) i64_const(code, simm);
            else local_get(code, 5 + src);
            code.push_back(0x85);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IXOR_M) {
            local_get(code, 5 + dst);
            const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero);
            code.push_back(0x85);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IROR_R) {
            local_get(code, 5 + dst);
            if (src == dst) i64_const(code, static_cast<int64_t>(ins.getImm32()));
            else local_get(code, 5 + src);
            code.push_back(0x88);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_IROL_R) {
            local_get(code, 5 + dst);
            if (src == dst) i64_const(code, static_cast<int64_t>(ins.getImm32()));
            else local_get(code, 5 + src);
            code.push_back(0x89);
            local_set(code, 5 + dst);
        }
        else if (op < ceil_ISWAP_R) {
            if (src != dst) {
                local_get(code, 5 + dst);
                local_get(code, 5 + src);
                local_set(code, 5 + dst);
                local_set(code, 5 + src);
            }
        }
        else if (op < ceil_FSWAP_R) {
            const int target = (ins.dst % RegistersCount) < RegisterCountFlt
                                   ? 13 + fdst : 17 + fdst;
            local_get(code, target);
            local_get(code, target);
            v128_op(code, 0x0d);
            const uint8_t shuffle[16] = {8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7};
            code.insert(code.end(), shuffle, shuffle + 16);
            local_set(code, target);
        }
        else if (op < ceil_FADD_R) {
            local_get(code, 13 + fdst);
            local_get(code, 21 + fsrc);
            v128_op(code, 0xf0);
            local_set(code, 13 + fdst);
        }
        else if (op < ceil_FSUB_R) {
            return false;
        }
        else if (op < ceil_FSUB_M) {
            local_get(code, 13 + fdst);
            local_get(code, 21 + fsrc);
            v128_op(code, 0xf1);
            local_set(code, 13 + fdst);
        }
        else if (op < ceil_FSCAL_R) {
            return false;
        }
        else if (op < ceil_FMUL_R) {
            local_get(code, 13 + fdst);
            v128_op(code, 0x0c);
            const uint64_t mask = 0x80F0000000000000ULL;
            for (int lane = 0; lane < 2; ++lane) {
                for (int b = 0; b < 8; ++b)
                    code.push_back(static_cast<uint8_t>(mask >> (8 * b)));
            }
            v128_op(code, 0x51);
            local_set(code, 13 + fdst);
        }
        else if (op < ceil_FDIV_M) {
            local_get(code, 17 + fdst);
            local_get(code, 21 + fsrc);
            v128_op(code, 0xf2);
            local_set(code, 17 + fdst);
        }
        else if (op < ceil_FSQRT_R) {
            return false;
        }
        else if (op < ceil_CBRANCH) {
            local_get(code, 17 + fdst);
            v128_op(code, 0xef);
            local_set(code, 17 + fdst);
        }
        else if (op < ceil_ISTORE) {
            return false;
        }
        else if (op < ceil_NOP) {
            const uint32_t mask = (ins.getModCond() >= StoreL3Condition)
                                      ? ScratchpadL3Mask
                                      : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_address(code, static_cast<uint32_t>(dst), simm, mask, false);
            local_get(code, 5 + src);
            i64_store(code);
        }
    }

    for (int r = 0; r < RegistersCount; ++r) {
        local_get(code, static_cast<uint32_t>(5 + r));
        store_reg(code, static_cast<uint32_t>(r));
    }
    for (int r = 0; r < RegisterCountFlt; ++r) {
        local_get(code, static_cast<uint32_t>(13 + r));
        store_fp(code, 1, static_cast<uint32_t>(r));
        local_get(code, static_cast<uint32_t>(17 + r));
        store_fp(code, 2, static_cast<uint32_t>(r));
        local_get(code, static_cast<uint32_t>(21 + r));
        store_fp(code, 3, static_cast<uint32_t>(r));
    }
    code.push_back(0x0b);

    module_.insert(module_.end(), {0x00, 0x61, 0x73, 0x6d,
                                   0x01, 0x00, 0x00, 0x00});

    std::vector<uint8_t> type;
    uleb(type, 3);
    type.push_back(0x60);
    uleb(type, 5);
    for (int i = 0; i < 5; ++i) type.push_back(0x7f);
    uleb(type, 0);
    for (int t = 0; t < 2; ++t) {
        type.push_back(0x60);
        uleb(type, 2);
        type.push_back(0x7e);
        type.push_back(0x7e);
        uleb(type, 1);
        type.push_back(0x7e);
    }
    section(module_, 1, type);

    std::vector<uint8_t> imports;
    uleb(imports, 3);
    bytes(imports, "env", 3);
    bytes(imports, "mulh_u64", 8);
    imports.push_back(0x00);
    uleb(imports, 1);
    bytes(imports, "env", 3);
    bytes(imports, "mulh_s64", 8);
    imports.push_back(0x00);
    uleb(imports, 2);
    bytes(imports, "env", 3);
    bytes(imports, "memory", 6);
    imports.push_back(0x02);
    imports.push_back(0x00); // imported memory: minimum only
    uleb(imports, 1);
    section(module_, 2, imports);

    std::vector<uint8_t> funcs;
    uleb(funcs, 1);
    uleb(funcs, 0);
    section(module_, 3, funcs);

    std::vector<uint8_t> exports;
    uleb(exports, 1);
    bytes(exports, "rx_jit", 6);
    exports.push_back(0x00);
    uleb(exports, 2);
    section(module_, 7, exports);

    std::vector<uint8_t> body;
    uleb(body, 3);
    uleb(body, 8);
    body.push_back(0x7e);
    uleb(body, 12);
    body.push_back(0x7b);
    body.insert(body.end(), code.begin(), code.end());

    std::vector<uint8_t> codes;
    uleb(codes, 1);
    uleb(codes, static_cast<uint32_t>(body.size()));
    codes.insert(codes.end(), body.begin(), body.end());
    section(module_, 10, codes);

    return true;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
