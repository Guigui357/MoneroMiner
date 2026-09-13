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
    c.push_back(0x20); uleb(c, index);
}

static void local_set(std::vector<uint8_t>& c, uint32_t index) {
    c.push_back(0x21); uleb(c, index);
}

static void i32_const(std::vector<uint8_t>& c, int32_t value) {
    c.push_back(0x41); sleb(c, value);
}

static void i64_const(std::vector<uint8_t>& c, int64_t value) {
    c.push_back(0x42); sleb(c, value);
}

static void i64_load(std::vector<uint8_t>& c) {
    c.push_back(0x29); uleb(c, 3); uleb(c, 0);
}

static void i32_load(std::vector<uint8_t>& c, uint32_t offset = 0) {
    c.push_back(0x28); uleb(c, 2); uleb(c, offset);
}

static void i64_store(std::vector<uint8_t>& c) {
    c.push_back(0x37); uleb(c, 3); uleb(c, 0);
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

// Exact IEEE-754 binary64 bit patterns are stored in locals.
// F: 13..20, E: 21..28, A: 29..36, fprc: 37.
static void load_fp_lane(std::vector<uint8_t>& c, uint32_t ptr_local,
                         uint32_t index, uint32_t lane) {
    local_get(c, ptr_local);
    i32_const(c, static_cast<int32_t>(index * 16 + lane * 8));
    c.push_back(0x6a);
    i64_load(c);
}

static void store_fp_lane(std::vector<uint8_t>& c, uint32_t ptr_local,
                          uint32_t index, uint32_t lane) {
    local_get(c, ptr_local);
    i32_const(c, static_cast<int32_t>(index * 16 + lane * 8));
    c.push_back(0x6a);
    i64_store(c);
}

static int f_local(int group, int reg, int lane) {
    return 13 + group * 8 + reg * 2 + lane;
}

static void emit_address(std::vector<uint8_t>& c, uint32_t src,
                         int64_t imm, uint32_t mask, bool zero_register) {
    local_get(c, 4);
    if (zero_register) {
        i64_const(c, imm);
    } else {
        local_get(c, 5 + src);
        i64_const(c, imm);
        c.push_back(0x7c);
    }
    i64_const(c, static_cast<int64_t>(mask));
    c.push_back(0x83);
    c.push_back(0xa7);
    c.push_back(0x6a);
}

static void emit_load(std::vector<uint8_t>& c, uint32_t src,
                      int64_t imm, uint32_t mask, bool zero_register) {
    emit_address(c, src, imm, mask, zero_register);
    i64_load(c);
}

static void emit_call(std::vector<uint8_t>& c, uint32_t fn) {
    c.push_back(0x10); uleb(c, fn);
}

static void emit_fp_bin(std::vector<uint8_t>& c, uint32_t fn,
                        int lhs, int rhs, int fprc_local) {
    local_get(c, lhs);
    local_get(c, rhs);
    local_get(c, fprc_local);
    emit_call(c, fn);
}

static void emit_fp_sqrt(std::vector<uint8_t>& c, uint32_t fn,
                         int value, int fprc_local) {
    local_get(c, value);
    local_get(c, fprc_local);
    emit_call(c, fn);
}

static void emit_fmem_i32(std::vector<uint8_t>& c, uint32_t src,
                          int64_t imm, uint32_t mask, bool zero,
                          uint32_t lane) {
    emit_address(c, src, imm, mask, zero);
    i32_load(c, lane * 4);
    emit_call(c, 7);
}

static int reg(uint8_t r) { return static_cast<int>(r % RegistersCount); }
static int flt(uint8_t r) { return static_cast<int>(r % RegisterCountFlt); }

static bool supported(const Instruction& ins) {
    const int op = ins.opcode;
    if (op < ceil_FSWAP_R) return true;
    if (op < ceil_FADD_R) return true;
    if (op < ceil_FADD_M) return true;
    if (op < ceil_FSUB_R) return true;
    if (op < ceil_FSUB_M) return true;
    if (op < ceil_FSCAL_R) return true;
    if (op < ceil_FMUL_R) return true;
    if (op < ceil_FDIV_M) return true;
    if (op < ceil_FSQRT_R) return true;
    if (op < ceil_CBRANCH) return true;
    if (op < ceil_CFROUND) return false;
    if (op < ceil_ISTORE) return true;
    if (op < ceil_NOP) return true;
    return true;
}

} // namespace

bool WasmJit::compile(const Program& program) {
    module_.clear();

    for (uint32_t pc = 0; pc < program.getSize(); ++pc) {
        const Instruction& ins = program(static_cast<int>(pc));
        if (!supported(ins)) {
            std::cout << "[WASM-JIT] UNSUPPORTED pc=" << pc
                      << " opcode=" << static_cast<int>(ins.opcode) << std::endl;
            return false;
        }
    }

    std::vector<uint8_t> code;

    for (int r = 0; r < RegistersCount; ++r) {
        load_reg(code, static_cast<uint32_t>(r));
        local_set(code, static_cast<uint32_t>(5 + r));
    }
    for (int r = 0; r < RegisterCountFlt; ++r) {
        for (int lane = 0; lane < 2; ++lane) {
            load_fp_lane(code, 1, static_cast<uint32_t>(r), lane);
            local_set(code, static_cast<uint32_t>(f_local(0, r, lane)));
            load_fp_lane(code, 2, static_cast<uint32_t>(r), lane);
            local_set(code, static_cast<uint32_t>(f_local(1, r, lane)));
            load_fp_lane(code, 3, static_cast<uint32_t>(r), lane);
            local_set(code, static_cast<uint32_t>(f_local(2, r, lane)));
        }
    }

    i32_const(code, 0);
    local_set(code, 37);

    for (uint32_t pc = 0; pc < program.getSize(); ++pc) {
        const Instruction& ins = program(static_cast<int>(pc));
        const int op = ins.opcode;
        const int dst = reg(ins.dst), src = reg(ins.src);
        const int fdst = flt(ins.dst), fsrc = flt(ins.src);
        const int64_t simm = static_cast<int64_t>(static_cast<int32_t>(ins.getImm32()));

        if (op < ceil_IADD_RS) {
            local_get(code, 5 + dst); local_get(code, 5 + src);
            i64_const(code, static_cast<int64_t>(ins.getModShift())); code.push_back(0x86);
            if (dst == RegisterNeedsDisplacement) { i64_const(code, simm); code.push_back(0x7c); }
            code.push_back(0x7c); local_set(code, 5 + dst);
        }
        else if (op < ceil_IADD_M) {
            local_get(code, 5 + dst); const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero); code.push_back(0x7c); local_set(code, 5 + dst);
        }
        else if (op < ceil_ISUB_R) {
            local_get(code, 5 + dst); if (src == dst) i64_const(code, simm); else local_get(code, 5 + src);
            code.push_back(0x7d); local_set(code, 5 + dst);
        }
        else if (op < ceil_ISUB_M) {
            local_get(code, 5 + dst); const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero); code.push_back(0x7d); local_set(code, 5 + dst);
        }
        else if (op < ceil_IMUL_R) {
            local_get(code, 5 + dst); if (src == dst) i64_const(code, simm); else local_get(code, 5 + src);
            code.push_back(0x7e); local_set(code, 5 + dst);
        }
        else if (op < ceil_IMUL_M) {
            local_get(code, 5 + dst); const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero); code.push_back(0x7e); local_set(code, 5 + dst);
        }
        else if (op < ceil_IMULH_R) {
            local_get(code, 5 + dst); local_get(code, 5 + src); emit_call(code, 0); local_set(code, 5 + dst);
        }
        else if (op < ceil_IMULH_M) {
            local_get(code, 5 + dst); const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero); emit_call(code, 0); local_set(code, 5 + dst);
        }
        else if (op < ceil_ISMULH_R) {
            local_get(code, 5 + dst); local_get(code, 5 + src); emit_call(code, 1); local_set(code, 5 + dst);
        }
        else if (op < ceil_ISMULH_M) {
            local_get(code, 5 + dst); const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero); emit_call(code, 1); local_set(code, 5 + dst);
        }
        else if (op < ceil_IMUL_RCP) {
            const uint32_t divisor = ins.getImm32();
            if (!isZeroOrPowerOf2(divisor)) { local_get(code, 5 + dst); i64_const(code, static_cast<int64_t>(randomx_reciprocal(divisor))); code.push_back(0x7e); local_set(code, 5 + dst); }
        }
        else if (op < ceil_INEG_R) {
            local_get(code, 5 + dst); i64_const(code, 0); code.push_back(0x7d); local_set(code, 5 + dst);
        }
        else if (op < ceil_IXOR_R) {
            local_get(code, 5 + dst); if (src == dst) i64_const(code, simm); else local_get(code, 5 + src); code.push_back(0x85); local_set(code, 5 + dst);
        }
        else if (op < ceil_IXOR_M) {
            local_get(code, 5 + dst); const bool zero = src == dst;
            const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_load(code, static_cast<uint32_t>(src), simm, mask, zero); code.push_back(0x85); local_set(code, 5 + dst);
        }
        else if (op < ceil_IROR_R) {
            local_get(code, 5 + dst); if (src == dst) i64_const(code, static_cast<int64_t>(ins.getImm32())); else local_get(code, 5 + src); code.push_back(0x88); local_set(code, 5 + dst);
        }
        else if (op < ceil_IROL_R) {
            local_get(code, 5 + dst); if (src == dst) i64_const(code, static_cast<int64_t>(ins.getImm32())); else local_get(code, 5 + src); code.push_back(0x89); local_set(code, 5 + dst);
        }
        else if (op < ceil_ISWAP_R) {
            if (src != dst) { local_get(code, 5 + dst); local_get(code, 5 + src); local_set(code, 5 + dst); local_set(code, 5 + src); }
        }
        else if (op < ceil_FSWAP_R) {
            const int group = (ins.dst % RegistersCount) < RegisterCountFlt ? 0 : 1;
            local_get(code, f_local(group, fdst, 0)); local_get(code, f_local(group, fdst, 1));
            local_set(code, f_local(group, fdst, 0)); local_set(code, f_local(group, fdst, 1));
        }
        else if (op < ceil_FADD_R) {
            emit_fp_bin(code, 2, f_local(0, fdst, 0), f_local(2, fsrc, 0), 37); local_set(code, f_local(0, fdst, 0));
            emit_fp_bin(code, 2, f_local(0, fdst, 1), f_local(2, fsrc, 1), 37); local_set(code, f_local(0, fdst, 1));
        }
        else if (op < ceil_FADD_M) {
            const bool zero = src == dst; const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            for (int lane = 0; lane < 2; ++lane) { local_get(code, f_local(0, fdst, lane)); emit_fmem_i32(code, static_cast<uint32_t>(src), simm, mask, zero, lane); local_get(code, 37); emit_call(code, 2); local_set(code, f_local(0, fdst, lane)); }
        }
        else if (op < ceil_FSUB_R) {
            emit_fp_bin(code, 3, f_local(0, fdst, 0), f_local(2, fsrc, 0), 37); local_set(code, f_local(0, fdst, 0));
            emit_fp_bin(code, 3, f_local(0, fdst, 1), f_local(2, fsrc, 1), 37); local_set(code, f_local(0, fdst, 1));
        }
        else if (op < ceil_FSUB_M) {
            const bool zero = src == dst; const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            for (int lane = 0; lane < 2; ++lane) { local_get(code, f_local(0, fdst, lane)); emit_fmem_i32(code, static_cast<uint32_t>(src), simm, mask, zero, lane); local_get(code, 37); emit_call(code, 3); local_set(code, f_local(0, fdst, lane)); }
        }
        else if (op < ceil_FSCAL_R) {
            const uint64_t mask = 0x80F0000000000000ULL;
            for (int lane = 0; lane < 2; ++lane) { local_get(code, f_local(0, fdst, lane)); i64_const(code, static_cast<int64_t>(mask)); code.push_back(0x85); local_set(code, f_local(0, fdst, lane)); }
        }
        else if (op < ceil_FMUL_R) {
            emit_fp_bin(code, 4, f_local(1, fdst, 0), f_local(2, fsrc, 0), 37); local_set(code, f_local(1, fdst, 0));
            emit_fp_bin(code, 4, f_local(1, fdst, 1), f_local(2, fsrc, 1), 37); local_set(code, f_local(1, fdst, 1));
        }
        else if (op < ceil_FDIV_M) {
            const bool zero = src == dst; const uint32_t mask = zero ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            for (int lane = 0; lane < 2; ++lane) { local_get(code, f_local(1, fdst, lane)); emit_fmem_i32(code, static_cast<uint32_t>(src), simm, mask, zero, lane); local_get(code, 37); emit_call(code, 5); local_set(code, f_local(1, fdst, lane)); }
        }
        else if (op < ceil_FSQRT_R) {
            emit_fp_sqrt(code, 6, f_local(1, fdst, 0), 37); local_set(code, f_local(1, fdst, 0));
            emit_fp_sqrt(code, 6, f_local(1, fdst, 1), 37); local_set(code, f_local(1, fdst, 1));
        }
        else if (op < ceil_CBRANCH) {
            return false;
        }
        else if (op < ceil_CFROUND) {
            local_get(code, 5 + src); i64_const(code, static_cast<int64_t>(ins.getImm32() & 63)); code.push_back(0x8a);
            i64_const(code, 0x3c); code.push_back(0x83); code.push_back(0x50); code.push_back(0x04); code.push_back(0x40);
            local_get(code, 5 + src); i64_const(code, static_cast<int64_t>(ins.getImm32() & 63)); code.push_back(0x8a);
            i64_const(code, 3); code.push_back(0x83); code.push_back(0xa7); local_set(code, 37); code.push_back(0x0b);
        }
        else if (op < ceil_ISTORE) {
            const uint32_t mask = (ins.getModCond() >= StoreL3Condition) ? ScratchpadL3Mask : (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            emit_address(code, static_cast<uint32_t>(dst), simm, mask, false); local_get(code, 5 + src); i64_store(code);
        }
        else if (op < ceil_NOP) {
        }
    }

    for (int r = 0; r < RegistersCount; ++r) { local_get(code, static_cast<uint32_t>(5 + r)); store_reg(code, static_cast<uint32_t>(r)); }
    for (int r = 0; r < RegisterCountFlt; ++r) for (int lane = 0; lane < 2; ++lane) {
        local_get(code, f_local(0, r, lane)); store_fp_lane(code, 1, static_cast<uint32_t>(r), lane);
        local_get(code, f_local(1, r, lane)); store_fp_lane(code, 2, static_cast<uint32_t>(r), lane);
        local_get(code, f_local(2, r, lane)); store_fp_lane(code, 3, static_cast<uint32_t>(r), lane);
    }
    code.push_back(0x0b);

    module_.insert(module_.end(), {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00});

    // 0=rx_jit, 1=mulh(i64,i64)->i64, 2=fp_bin(i64,i64,i32)->i64,
    // 3=fp_unary(i64,i32)->i64, 4=fp_from_i32(i32)->i64.
    std::vector<uint8_t> type; uleb(type, 5);
    type.push_back(0x60); uleb(type, 5); for (int i = 0; i < 5; ++i) type.push_back(0x7f); uleb(type, 0);
    type.push_back(0x60); uleb(type, 2); type.push_back(0x7e); type.push_back(0x7e); uleb(type, 1); type.push_back(0x7e);
    type.push_back(0x60); uleb(type, 3); type.push_back(0x7e); type.push_back(0x7e); type.push_back(0x7f); uleb(type, 1); type.push_back(0x7e);
    type.push_back(0x60); uleb(type, 2); type.push_back(0x7e); type.push_back(0x7f); uleb(type, 1); type.push_back(0x7e);
    type.push_back(0x60); uleb(type, 1); type.push_back(0x7f); uleb(type, 1); type.push_back(0x7e);
    section(module_, 1, type);

    std::vector<uint8_t> imports; uleb(imports, 9);
    bytes(imports, "env", 3); bytes(imports, "mulh_u64", 8); imports.push_back(0x00); uleb(imports, 1);
    bytes(imports, "env", 3); bytes(imports, "mulh_s64", 8); imports.push_back(0x00); uleb(imports, 1);
    bytes(imports, "env", 3); bytes(imports, "fp_add", 6); imports.push_back(0x00); uleb(imports, 2);
    bytes(imports, "env", 3); bytes(imports, "fp_sub", 6); imports.push_back(0x00); uleb(imports, 2);
    bytes(imports, "env", 3); bytes(imports, "fp_mul", 6); imports.push_back(0x00); uleb(imports, 2);
    bytes(imports, "env", 3); bytes(imports, "fp_div", 6); imports.push_back(0x00); uleb(imports, 2);
    bytes(imports, "env", 3); bytes(imports, "fp_sqrt", 7); imports.push_back(0x00); uleb(imports, 3);
    bytes(imports, "env", 3); bytes(imports, "fp_from_i32", 11); imports.push_back(0x00); uleb(imports, 4);
    bytes(imports, "env", 3); bytes(imports, "memory", 6); imports.push_back(0x02); imports.push_back(0x00); uleb(imports, 1);
    section(module_, 2, imports);

    std::vector<uint8_t> funcs; uleb(funcs, 1); uleb(funcs, 0); section(module_, 3, funcs);
    std::vector<uint8_t> exports; uleb(exports, 1); bytes(exports, "rx_jit", 6); exports.push_back(0x00); uleb(exports, 8); section(module_, 7, exports);

    std::vector<uint8_t> body; uleb(body, 3);
    uleb(body, 8); body.push_back(0x7e);
    uleb(body, 24); body.push_back(0x7e);
    uleb(body, 1); body.push_back(0x7f);
    body.insert(body.end(), code.begin(), code.end());

    std::vector<uint8_t> codes; uleb(codes, 1); uleb(codes, static_cast<uint32_t>(body.size()));
    codes.insert(codes.end(), body.begin(), body.end()); section(module_, 10, codes);
    return true;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
