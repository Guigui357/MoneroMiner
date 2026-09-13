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

static void emit_address(std::vector<uint8_t>& c, uint32_t src,
                         int64_t imm, uint32_t mask) {
    local_get(c, 1);
    local_get(c, 2 + src);
    i64_const(c, imm);
    c.push_back(0x7c);
    i64_const(c, static_cast<int64_t>(mask));
    c.push_back(0x83);
    c.push_back(0xa7);
    c.push_back(0x6a);
}

static void emit_load(std::vector<uint8_t>& c, uint32_t src,
                      int64_t imm, uint32_t mask) {
    emit_address(c, src, imm, mask);
    i64_load(c);
}

static void emit_store(std::vector<uint8_t>& c, uint32_t src,
                       int64_t imm, uint32_t mask) {
    emit_address(c, src, imm, mask);
    i64_store(c);
}

static int reg(uint8_t r) {
    return static_cast<int>(r % RegistersCount);
}

static bool supported(const Instruction& ins) {
    const int op = ins.opcode;
    if (op < ceil_IMULH_R) return true;
    if (op >= ceil_IMUL_RCP && op < ceil_FSWAP_R) return true;
    if (op >= ceil_NOP) return true;
    return false;
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
        local_set(code, static_cast<uint32_t>(2 + r));
    }

    for (uint32_t pc = 0; pc < program.getSize(); ++pc) {
        const Instruction& ins = program(static_cast<int>(pc));
        const int op = ins.opcode;
        const int dst = reg(ins.dst);
        const int src = reg(ins.src);
        const int64_t simm = static_cast<int64_t>(static_cast<int32_t>(ins.getImm32()));

        if (op < ceil_IADD_RS) {
            local_get(code, 2 + dst);
            local_get(code, 2 + src);
            i64_const(code, static_cast<int64_t>(ins.getModShift()));
            code.push_back(0x86);
            if (dst == RegisterNeedsDisplacement) {
                i64_const(code, simm);
                code.push_back(0x7c);
            }
            code.push_back(0x7c);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IADD_M) {
            local_get(code, 2 + dst);
            const uint32_t mask = (src == dst) ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            const uint32_t address_reg = (src == dst) ? 0u : static_cast<uint32_t>(src);
            emit_load(code, address_reg, simm, mask);
            code.push_back(0x7c);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_ISUB_R) {
            local_get(code, 2 + dst);
            if (src == dst) i64_const(code, simm);
            else local_get(code, 2 + src);
            code.push_back(0x7d);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_ISUB_M) {
            local_get(code, 2 + dst);
            const uint32_t mask = (src == dst) ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            const uint32_t address_reg = (src == dst) ? 0u : static_cast<uint32_t>(src);
            emit_load(code, address_reg, simm, mask);
            code.push_back(0x7d);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IMUL_R) {
            local_get(code, 2 + dst);
            if (src == dst) i64_const(code, simm);
            else local_get(code, 2 + src);
            code.push_back(0x7e);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IMUL_M) {
            local_get(code, 2 + dst);
            const uint32_t mask = (src == dst) ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            const uint32_t address_reg = (src == dst) ? 0u : static_cast<uint32_t>(src);
            emit_load(code, address_reg, simm, mask);
            code.push_back(0x7e);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IMULH_R) {
            return false;
        }
        else if (op < ceil_IMUL_RCP) {
            return false;
        }
        else if (op < ceil_INEG_R) {
            const uint32_t divisor = ins.getImm32();
            if (!isZeroOrPowerOf2(divisor)) {
                local_get(code, 2 + dst);
                i64_const(code, static_cast<int64_t>(randomx_reciprocal(divisor)));
                code.push_back(0x7e);
                local_set(code, 2 + dst);
            }
        }
        else if (op < ceil_IXOR_R) {
            local_get(code, 2 + dst);
            i64_const(code, 0);
            code.push_back(0x7d);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IXOR_M) {
            local_get(code, 2 + dst);
            if (src == dst) i64_const(code, simm);
            else local_get(code, 2 + src);
            code.push_back(0x85);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IROR_R) {
            local_get(code, 2 + dst);
            const uint32_t mask = (src == dst) ? ScratchpadL3Mask :
                                  (ins.getModMem() ? ScratchpadL1Mask : ScratchpadL2Mask);
            const uint32_t address_reg = (src == dst) ? 0u : static_cast<uint32_t>(src);
            emit_load(code, address_reg, simm, mask);
            code.push_back(0x85);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_IROL_R) {
            local_get(code, 2 + dst);
            if (src == dst) i64_const(code, static_cast<int64_t>(ins.getImm32()));
            else local_get(code, 2 + src);
            code.push_back(0x8a);
            local_set(code, 2 + dst);
        }
        else if (op < ceil_ISWAP_R) {
            if (src != dst) {
                local_get(code, 2 + dst);
                local_get(code, 2 + src);
                local_set(code, 2 + dst);
                local_set(code, 2 + src);
            }
        }
        else {
            return false;
        }
    }

    for (int r = 0; r < RegistersCount; ++r) {
        local_get(code, static_cast<uint32_t>(2 + r));
        store_reg(code, static_cast<uint32_t>(r));
    }
    code.push_back(0x0b);

    module_.insert(module_.end(), {0x00, 0x61, 0x73, 0x6d,
                                   0x01, 0x00, 0x00, 0x00});

    std::vector<uint8_t> type;
    uleb(type, 1);
    type.push_back(0x60);
    uleb(type, 2);
    type.push_back(0x7f);
    type.push_back(0x7f);
    uleb(type, 0);
    section(module_, 1, type);

    std::vector<uint8_t> imports;
    uleb(imports, 1);
    bytes(imports, "env", 3);
    bytes(imports, "memory", 6);
    imports.push_back(0x02);
    imports.push_back(0x00);
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
    exports.push_back(0x00);
    section(module_, 7, exports);

    std::vector<uint8_t> body;
    uleb(body, 1);
    uleb(body, 8);
    body.push_back(0x7e);
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
