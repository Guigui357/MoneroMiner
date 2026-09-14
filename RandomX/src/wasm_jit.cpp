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

        more = !((value == 0 && !sign) ||
                 (value == -1 && sign));

        if (more)
            b |= 0x80u;

        out.push_back(b);
    }
}

static void bytes(std::vector<uint8_t>& out,
                  const char* s,
                  size_t n) {
    uleb(out, static_cast<uint32_t>(n));
    out.insert(out.end(), s, s + n);
}

static void section(std::vector<uint8_t>& module,
                    uint8_t id,
                    const std::vector<uint8_t>& payload) {
    module.push_back(id);
    uleb(module, static_cast<uint32_t>(payload.size()));
    module.insert(module.end(), payload.begin(), payload.end());
}

static void local_get(std::vector<uint8_t>& c,
                      uint32_t index) {
    c.push_back(0x20);
    uleb(c, index);
}

static void local_set(std::vector<uint8_t>& c,
                      uint32_t index) {
    c.push_back(0x21);
    uleb(c, index);
}

static void i32_const(std::vector<uint8_t>& c,
                      int32_t value) {
    c.push_back(0x41);
    sleb(c, value);
}

static void i64_const(std::vector<uint8_t>& c,
                      int64_t value) {
    c.push_back(0x42);
    sleb(c, value);
}

static void i64_load(std::vector<uint8_t>& c) {
    c.push_back(0x29); // i64.load
    uleb(c, 3);        // align = 8
    uleb(c, 0);        // offset = 0
}

static void i32_load(std::vector<uint8_t>& c,
                     uint32_t offset = 0) {
    c.push_back(0x28); // i32.load
    uleb(c, 2);        // align = 4
    uleb(c, offset);
}

static void i64_store(std::vector<uint8_t>& c) {
    c.push_back(0x37); // i64.store
    uleb(c, 3);        // align = 8
    uleb(c, 0);        // offset = 0
}

static void load_reg(std::vector<uint8_t>& c,
                     uint32_t r) {
    local_get(c, 0);

    i32_const(
        c,
        static_cast<int32_t>(r * 8)
    );

    c.push_back(0x6a); // i32.add

    i64_load(c);
}

static void store_reg(std::vector<uint8_t>& c,
                      uint32_t r) {
    local_get(c, 0);

    i32_const(
        c,
        static_cast<int32_t>(r * 8)
    );

    c.push_back(0x6a); // i32.add

    i64_store(c);
}

// Exact IEEE-754 binary64 bit patterns are stored in locals.
//
// Parameters:
//   0 = register pointer
//   1 = F pointer
//   2 = E pointer
//   3 = A pointer
//   4 = scratchpad pointer
//
// Locals:
//   5..12  = integer registers
//   13..20 = F
//   21..28 = E
//   29..36 = A
//   37     = fprc
static void load_fp_lane(std::vector<uint8_t>& c,
                         uint32_t ptr_local,
                         uint32_t index,
                         uint32_t lane) {
    local_get(c, ptr_local);

    i32_const(
        c,
        static_cast<int32_t>(
            index * 16 + lane * 8
        )
    );

    c.push_back(0x6a); // i32.add

    i64_load(c);
}

static void store_fp_lane(std::vector<uint8_t>& c,
                          uint32_t ptr_local,
                          uint32_t index,
                          uint32_t lane) {
    local_get(c, ptr_local);

    i32_const(
        c,
        static_cast<int32_t>(
            index * 16 + lane * 8
        )
    );

    c.push_back(0x6a); // i32.add

    i64_store(c);
}

static int f_local(int group,
                   int reg,
                   int lane) {
    return 13 +
           group * 8 +
           reg * 2 +
           lane;
}

static void emit_address(std::vector<uint8_t>& c,
                         uint32_t src,
                         int64_t imm,
                         uint32_t mask,
                         bool zero_register) {
    local_get(c, 4);

    if (zero_register) {
        i64_const(c, imm);
    } else {
        local_get(c, 5 + src);
        i64_const(c, imm);
        c.push_back(0x7c); // i64.add
    }

    i64_const(
        c,
        static_cast<int64_t>(mask)
    );

    c.push_back(0x83); // i64.and

    c.push_back(0xa7); // i32.wrap_i64

    c.push_back(0x6a); // i32.add
}

static void emit_load(std::vector<uint8_t>& c,
                      uint32_t src,
                      int64_t imm,
                      uint32_t mask,
                      bool zero_register) {
    emit_address(
        c,
        src,
        imm,
        mask,
        zero_register
    );

    i64_load(c);
}

static void emit_call(std::vector<uint8_t>& c,
                      uint32_t fn) {
    c.push_back(0x10); // call
    uleb(c, fn);
}

static void emit_fp_bin(std::vector<uint8_t>& c,
                        uint32_t fn,
                        int lhs,
                        int rhs,
                        int fprc_local) {
    local_get(c, lhs);
    local_get(c, rhs);
    local_get(c, fprc_local);

    emit_call(c, fn);
}

static void emit_fp_sqrt(std::vector<uint8_t>& c,
                         uint32_t fn,
                         int value,
                         int fprc_local) {
    local_get(c, value);
    local_get(c, fprc_local);

    emit_call(c, fn);
}

static void emit_fmem_i32(std::vector<uint8_t>& c,
                          uint32_t src,
                          int64_t imm,
                          uint32_t mask,
                          bool zero,
                          uint32_t lane) {
    emit_address(
        c,
        src,
        imm,
        mask,
        zero
    );

    i32_load(
        c,
        lane * 4
    );

    emit_call(c, 7);
}

static int reg(uint8_t r) {
    return static_cast<int>(
        r % RegistersCount
    );
}

static int flt(uint8_t r) {
    return static_cast<int>(
        r % RegisterCountFlt
    );
}

static_assert(
    sizeof(Instruction::opcode) == sizeof(uint8_t),
    "Instruction::opcode must be uint8_t"
);

static const char* opcode_name(uint32_t op) {
    if (op < ceil_IADD_RS)  return "IADD_RS";
    if (op < ceil_IADD_M)   return "IADD_M";
    if (op < ceil_ISUB_R)   return "ISUB_R";
    if (op < ceil_ISUB_M)   return "ISUB_M";
    if (op < ceil_IMUL_R)   return "IMUL_R";
    if (op < ceil_IMUL_M)   return "IMUL_M";
    if (op < ceil_IMULH_R)  return "IMULH_R";
    if (op < ceil_IMULH_M)  return "IMULH_M";
    if (op < ceil_ISMULH_R) return "ISMULH_R";
    if (op < ceil_ISMULH_M) return "ISMULH_M";
    if (op < ceil_IMUL_RCP) return "IMUL_RCP";
    if (op < ceil_INEG_R)   return "INEG_R";
    if (op < ceil_IXOR_R)   return "IXOR_R";
    if (op < ceil_IXOR_M)   return "IXOR_M";
    if (op < ceil_IROR_R)   return "IROR_R";
    if (op < ceil_IROL_R)   return "IROL_R";
    if (op < ceil_ISWAP_R)  return "ISWAP_R";
    if (op < ceil_FSWAP_R)  return "FSWAP_R";
    if (op < ceil_FADD_R)   return "FADD_R";
    if (op < ceil_FADD_M)   return "FADD_M";
    if (op < ceil_FSUB_R)   return "FSUB_R";
    if (op < ceil_FSUB_M)   return "FSUB_M";
    if (op < ceil_FSCAL_R)  return "FSCAL_R";
    if (op < ceil_FMUL_R)   return "FMUL_R";
    if (op < ceil_FDIV_M)   return "FDIV_M";
    if (op < ceil_FSQRT_R)  return "FSQRT_R";
    if (op < ceil_CBRANCH)  return "CBRANCH";
    if (op < ceil_CFROUND)  return "CFROUND";
    if (op < ceil_ISTORE)   return "ISTORE";
    if (op < ceil_NOP)      return "NOP";

    return "INVALID";
}

static bool validate_opcode(uint32_t op,
                            uint32_t pc) {
    // An actual RandomX opcode is exactly one byte.
    if (op > 255u) {
        std::cout
            << "[WASM-JIT] INVALID OPCODE"
            << " pc=" << pc
            << " opcode=" << op
            << " (0x" << std::hex << op << std::dec << ")"
            << " -- impossible uint8_t value"
            << std::endl;

        return false;
    }

    return true;
}

static void print_opcode_limits() {
    std::cout
        << "[WASM-JIT] ===== RandomX opcode limits ====="
        << std::endl;

    std::cout << "[WASM-JIT] ceil_IADD_RS  = "
              << ceil_IADD_RS << std::endl;

    std::cout << "[WASM-JIT] ceil_IADD_M   = "
              << ceil_IADD_M << std::endl;

    std::cout << "[WASM-JIT] ceil_ISUB_R   = "
              << ceil_ISUB_R << std::endl;

    std::cout << "[WASM-JIT] ceil_ISUB_M   = "
              << ceil_ISUB_M << std::endl;

    std::cout << "[WASM-JIT] ceil_IMUL_R   = "
              << ceil_IMUL_R << std::endl;

    std::cout << "[WASM-JIT] ceil_IMUL_M   = "
              << ceil_IMUL_M << std::endl;

    std::cout << "[WASM-JIT] ceil_IMULH_R  = "
              << ceil_IMULH_R << std::endl;

    std::cout << "[WASM-JIT] ceil_IMULH_M  = "
              << ceil_IMULH_M << std::endl;

    std::cout << "[WASM-JIT] ceil_ISMULH_R = "
              << ceil_ISMULH_R << std::endl;

    std::cout << "[WASM-JIT] ceil_ISMULH_M = "
              << ceil_ISMULH_M << std::endl;

    std::cout << "[WASM-JIT] ceil_IMUL_RCP = "
              << ceil_IMUL_RCP << std::endl;

    std::cout << "[WASM-JIT] ceil_INEG_R   = "
              << ceil_INEG_R << std::endl;

    std::cout << "[WASM-JIT] ceil_IXOR_R   = "
              << ceil_IXOR_R << std::endl;

    std::cout << "[WASM-JIT] ceil_IXOR_M   = "
              << ceil_IXOR_M << std::endl;

    std::cout << "[WASM-JIT] ceil_IROR_R   = "
              << ceil_IROR_R << std::endl;

    std::cout << "[WASM-JIT] ceil_IROL_R   = "
              << ceil_IROL_R << std::endl;

    std::cout << "[WASM-JIT] ceil_ISWAP_R  = "
              << ceil_ISWAP_R << std::endl;

    std::cout << "[WASM-JIT] ceil_FSWAP_R  = "
              << ceil_FSWAP_R << std::endl;

    std::cout << "[WASM-JIT] ceil_FADD_R   = "
              << ceil_FADD_R << std::endl;

    std::cout << "[WASM-JIT] ceil_FADD_M   = "
              << ceil_FADD_M << std::endl;

    std::cout << "[WASM-JIT] ceil_FSUB_R   = "
              << ceil_FSUB_R << std::endl;

    std::cout << "[WASM-JIT] ceil_FSUB_M   = "
              << ceil_FSUB_M << std::endl;

    std::cout << "[WASM-JIT] ceil_FSCAL_R  = "
              << ceil_FSCAL_R << std::endl;

    std::cout << "[WASM-JIT] ceil_FMUL_R   = "
              << ceil_FMUL_R << std::endl;

    std::cout << "[WASM-JIT] ceil_FDIV_M   = "
              << ceil_FDIV_M << std::endl;

    std::cout << "[WASM-JIT] ceil_FSQRT_R  = "
              << ceil_FSQRT_R << std::endl;

    std::cout << "[WASM-JIT] ceil_CBRANCH  = "
              << ceil_CBRANCH << std::endl;

    std::cout << "[WASM-JIT] ceil_CFROUND  = "
              << ceil_CFROUND << std::endl;

    std::cout << "[WASM-JIT] ceil_ISTORE   = "
              << ceil_ISTORE << std::endl;

    std::cout << "[WASM-JIT] ceil_NOP      = "
              << ceil_NOP << std::endl;

    std::cout
        << "[WASM-JIT] ================================"
        << std::endl;
}

static bool supported(const Instruction& ins) {
    const uint32_t op =
        static_cast<uint32_t>(ins.opcode);

    // All valid RandomX opcodes are one byte.
    if (op > 255u)
        return false;

    /*
     * CBRANCH occupies:
     *
     *     ceil_FSQRT_R <= opcode < ceil_CBRANCH
     *
     * With the current RandomX configuration this is:
     *
     *     214 .. 238
     *
     * CFROUND starts at opcode 239.
     *
     * IMPORTANT:
     *
     * The old code used:
     *
     *     op >= ceil_CBRANCH && op < ceil_CFROUND
     *
     * which rejected CFROUND itself.
     */
    if (op >= ceil_FSQRT_R &&
        op < ceil_CBRANCH) {
        return false;
    }

    /*
     * ceil_NOP is 256 with the current configuration.
     * Since opcode is uint8_t, valid values are 0..255.
     */
    return true;
}

} // namespace


bool WasmJit::compile(const Program& program) {
    module_.clear();

    /*
     * ------------------------------------------------------------
     * PASS 1: validate all RandomX instructions
     * ------------------------------------------------------------
     */
    for (uint32_t pc = 0;
         pc < program.getSize();
         ++pc) {

        const Instruction& ins =
            program(static_cast<int>(pc));

        const uint32_t op =
            static_cast<uint32_t>(ins.opcode);

        if (!validate_opcode(op, pc))
            return false;

        if (!supported(ins)) {
            std::cout
                << "[WASM-JIT] UNSUPPORTED"
                << " pc=" << pc
                << " opcode=" << op
                << " (0x"
                << std::hex
                << op
                << std::dec
                << ")"
                << " instruction="
                << opcode_name(op)
                << std::endl;

            return false;
        }
    }


    std::vector<uint8_t> code;


    /*
     * ------------------------------------------------------------
     * Load integer registers
     * ------------------------------------------------------------
     */
    for (int r = 0;
         r < RegistersCount;
         ++r) {

        load_reg(
            code,
            static_cast<uint32_t>(r)
        );

        local_set(
            code,
            static_cast<uint32_t>(5 + r)
        );
    }


    /*
     * ------------------------------------------------------------
     * Load floating-point register groups
     * ------------------------------------------------------------
     */
    for (int r = 0;
         r < RegisterCountFlt;
         ++r) {

        for (int lane = 0;
             lane < 2;
             ++lane) {

            // F
            load_fp_lane(
                code,
                1,
                static_cast<uint32_t>(r),
                static_cast<uint32_t>(lane)
            );

            local_set(
                code,
                static_cast<uint32_t>(
                    f_local(0, r, lane)
                )
            );

            // E
            load_fp_lane(
                code,
                2,
                static_cast<uint32_t>(r),
                static_cast<uint32_t>(lane)
            );

            local_set(
                code,
                static_cast<uint32_t>(
                    f_local(1, r, lane)
                )
            );

            // A
            load_fp_lane(
                code,
                3,
                static_cast<uint32_t>(r),
                static_cast<uint32_t>(lane)
            );

            local_set(
                code,
                static_cast<uint32_t>(
                    f_local(2, r, lane)
                )
            );
        }
    }


    /*
     * ------------------------------------------------------------
     * Initialize fprc
     * ------------------------------------------------------------
     */
    i32_const(code, 0);
    local_set(code, 37);


    /*
     * ------------------------------------------------------------
     * Compile RandomX program
     * ------------------------------------------------------------
     */
    for (uint32_t pc = 0;
         pc < program.getSize();
         ++pc) {

        const Instruction& ins =
            program(static_cast<int>(pc));

        const int op = ins.opcode;

        const int dst =
            reg(ins.dst);

        const int src =
            reg(ins.src);

        const int fdst =
            flt(ins.dst);

        const int fsrc =
            flt(ins.src);

        const int64_t simm =
            static_cast<int64_t>(
                static_cast<int32_t>(
                    ins.getImm32()
                )
            );


        /*
         * --------------------------------------------------------
         * IADD_RS
         * --------------------------------------------------------
         */
        if (op < ceil_IADD_RS) {

            local_get(code, 5 + dst);
            local_get(code, 5 + src);

            i64_const(
                code,
                static_cast<int64_t>(
                    ins.getModShift()
                )
            );

            c.push_back(0x86); // i64.shl

            if (dst == RegisterNeedsDisplacement) {
                i64_const(code, simm);
                c.push_back(0x7c); // i64.add
            }

            c.push_back(0x7c); // i64.add

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IADD_M
         * --------------------------------------------------------
         */
        else if (op < ceil_IADD_M) {

            local_get(
                code,
                5 + dst
            );

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_load(
                code,
                static_cast<uint32_t>(src),
                simm,
                mask,
                zero
            );

            c.push_back(0x7c); // i64.add

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * ISUB_R
         * --------------------------------------------------------
         */
        else if (op < ceil_ISUB_R) {

            local_get(
                code,
                5 + dst
            );

            if (src == dst)
                i64_const(code, simm);
            else
                local_get(code, 5 + src);

            c.push_back(0x7d); // i64.sub

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * ISUB_M
         * --------------------------------------------------------
         */
        else if (op < ceil_ISUB_M) {

            local_get(
                code,
                5 + dst
            );

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_load(
                code,
                static_cast<uint32_t>(src),
                simm,
                mask,
                zero
            );

            c.push_back(0x7d); // i64.sub

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IMUL_R
         * --------------------------------------------------------
         */
        else if (op < ceil_IMUL_R) {

            local_get(
                code,
                5 + dst
            );

            if (src == dst)
                i64_const(code, simm);
            else
                local_get(code, 5 + src);

            c.push_back(0x7e); // i64.mul

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IMUL_M
         * --------------------------------------------------------
         */
        else if (op < ceil_IMUL_M) {

            local_get(
                code,
                5 + dst
            );

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_load(
                code,
                static_cast<uint32_t>(src),
                simm,
                mask,
                zero
            );

            c.push_back(0x7e); // i64.mul

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IMULH_R
         * --------------------------------------------------------
         */
        else if (op < ceil_IMULH_R) {

            local_get(code, 5 + dst);
            local_get(code, 5 + src);

            emit_call(code, 0);

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IMULH_M
         * --------------------------------------------------------
         */
        else if (op < ceil_IMULH_M) {

            local_get(code, 5 + dst);

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_load(
                code,
                static_cast<uint32_t>(src),
                simm,
                mask,
                zero
            );

            emit_call(code, 0);

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * ISMULH_R
         * --------------------------------------------------------
         */
        else if (op < ceil_ISMULH_R) {

            local_get(code, 5 + dst);
            local_get(code, 5 + src);

            emit_call(code, 1);

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * ISMULH_M
         * --------------------------------------------------------
         */
        else if (op < ceil_ISMULH_M) {

            local_get(code, 5 + dst);

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_load(
                code,
                static_cast<uint32_t>(src),
                simm,
                mask,
                zero
            );

            emit_call(code, 1);

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IMUL_RCP
         * --------------------------------------------------------
         */
        else if (op < ceil_IMUL_RCP) {

            const uint32_t divisor =
                ins.getImm32();

            if (!isZeroOrPowerOf2(divisor)) {

                local_get(
                    code,
                    5 + dst
                );

                i64_const(
                    code,
                    static_cast<int64_t>(
                        randomx_reciprocal(divisor)
                    )
                );

                c.push_back(0x7e); // i64.mul

                local_set(
                    code,
                    5 + dst
                );
            }
        }


        /*
         * --------------------------------------------------------
         * INEG_R
         * --------------------------------------------------------
         */
        else if (op < ceil_INEG_R) {

            local_get(
                code,
                5 + dst
            );

            i64_const(code, 0);

            c.push_back(0x7d); // i64.sub

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IXOR_R
         * --------------------------------------------------------
         */
        else if (op < ceil_IXOR_R) {

            local_get(
                code,
                5 + dst
            );

            if (src == dst)
                i64_const(code, simm);
            else
                local_get(code, 5 + src);

            c.push_back(0x85); // i64.xor

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IXOR_M
         * --------------------------------------------------------
         */
        else if (op < ceil_IXOR_M) {

            local_get(
                code,
                5 + dst
            );

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_load(
                code,
                static_cast<uint32_t>(src),
                simm,
                mask,
                zero
            );

            c.push_back(0x85); // i64.xor

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IROR_R
         * --------------------------------------------------------
         */
        else if (op < ceil_IROR_R) {

            local_get(
                code,
                5 + dst
            );

            if (src == dst)
                i64_const(
                    code,
                    static_cast<int64_t>(
                        ins.getImm32()
                    )
                );
            else
                local_get(
                    code,
                    5 + src
                );

            c.push_back(0x88); // i64.rotr

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * IROL_R
         * --------------------------------------------------------
         */
        else if (op < ceil_IROL_R) {

            local_get(
                code,
                5 + dst
            );

            if (src == dst)
                i64_const(
                    code,
                    static_cast<int64_t>(
                        ins.getImm32()
                    )
                );
            else
                local_get(
                    code,
                    5 + src
                );

            c.push_back(0x89); // i64.rotl

            local_set(
                code,
                5 + dst
            );
        }


        /*
         * --------------------------------------------------------
         * ISWAP_R
         * --------------------------------------------------------
         */
        else if (op < ceil_ISWAP_R) {

            if (src != dst) {

                local_get(
                    code,
                    5 + dst
                );

                local_get(
                    code,
                    5 + src
                );

                local_set(
                    code,
                    5 + dst
                );

                local_set(
                    code,
                    5 + src
                );
            }
        }


        /*
         * --------------------------------------------------------
         * FSWAP_R
         * --------------------------------------------------------
         */
        else if (op < ceil_FSWAP_R) {

            const int group =
                (ins.dst % RegistersCount) <
                RegisterCountFlt
                    ? 0
                    : 1;

            local_get(
                code,
                f_local(group, fdst, 0)
            );

            local_get(
                code,
                f_local(group, fdst, 1)
            );

            local_set(
                code,
                f_local(group, fdst, 0)
            );

            local_set(
                code,
                f_local(group, fdst, 1)
            );
        }


        /*
         * --------------------------------------------------------
         * FADD_R
         * --------------------------------------------------------
         */
        else if (op < ceil_FADD_R) {

            emit_fp_bin(
                code,
                2,
                f_local(0, fdst, 0),
                f_local(2, fsrc, 0),
                37
            );

            local_set(
                code,
                f_local(0, fdst, 0)
            );

            emit_fp_bin(
                code,
                2,
                f_local(0, fdst, 1),
                f_local(2, fsrc, 1),
                37
            );

            local_set(
                code,
                f_local(0, fdst, 1)
            );
        }


        /*
         * --------------------------------------------------------
         * FADD_M
         * --------------------------------------------------------
         */
        else if (op < ceil_FADD_M) {

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            for (int lane = 0;
                 lane < 2;
                 ++lane) {

                local_get(
                    code,
                    f_local(0, fdst, lane)
                );

                emit_fmem_i32(
                    code,
                    static_cast<uint32_t>(src),
                    simm,
                    mask,
                    zero,
                    static_cast<uint32_t>(lane)
                );

                local_get(code, 37);

                emit_call(code, 2);

                local_set(
                    code,
                    f_local(0, fdst, lane)
                );
            }
        }


        /*
         * --------------------------------------------------------
         * FSUB_R
         * --------------------------------------------------------
         */
        else if (op < ceil_FSUB_R) {

            emit_fp_bin(
                code,
                3,
                f_local(0, fdst, 0),
                f_local(2, fsrc, 0),
                37
            );

            local_set(
                code,
                f_local(0, fdst, 0)
            );

            emit_fp_bin(
                code,
                3,
                f_local(0, fdst, 1),
                f_local(2, fsrc, 1),
                37
            );

            local_set(
                code,
                f_local(0, fdst, 1)
            );
        }


        /*
         * --------------------------------------------------------
         * FSUB_M
         * --------------------------------------------------------
         */
        else if (op < ceil_FSUB_M) {

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            for (int lane = 0;
                 lane < 2;
                 ++lane) {

                local_get(
                    code,
                    f_local(0, fdst, lane)
                );

                emit_fmem_i32(
                    code,
                    static_cast<uint32_t>(src),
                    simm,
                    mask,
                    zero,
                    static_cast<uint32_t>(lane)
                );

                local_get(code, 37);

                emit_call(code, 3);

                local_set(
                    code,
                    f_local(0, fdst, lane)
                );
            }
        }


        /*
         * --------------------------------------------------------
         * FSCAL_R
         * --------------------------------------------------------
         */
        else if (op < ceil_FSCAL_R) {

            const uint64_t mask =
                0x80F0000000000000ULL;

            for (int lane = 0;
                 lane < 2;
                 ++lane) {

                local_get(
                    code,
                    f_local(0, fdst, lane)
                );

                i64_const(
                    code,
                    static_cast<int64_t>(mask)
                );

                c.push_back(0x85); // i64.xor

                local_set(
                    code,
                    f_local(0, fdst, lane)
                );
            }
        }


        /*
         * --------------------------------------------------------
         * FMUL_R
         * --------------------------------------------------------
         */
        else if (op < ceil_FMUL_R) {

            emit_fp_bin(
                code,
                4,
                f_local(1, fdst, 0),
                f_local(2, fsrc, 0),
                37
            );

            local_set(
                code,
                f_local(1, fdst, 0)
            );

            emit_fp_bin(
                code,
                4,
                f_local(1, fdst, 1),
                f_local(2, fsrc, 1),
                37
            );

            local_set(
                code,
                f_local(1, fdst, 1)
            );
        }


        /*
         * --------------------------------------------------------
         * FDIV_M
         * --------------------------------------------------------
         */
        else if (op < ceil_FDIV_M) {

            const bool zero =
                src == dst;

            const uint32_t mask =
                zero
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            for (int lane = 0;
                 lane < 2;
                 ++lane) {

                local_get(
                    code,
                    f_local(1, fdst, lane)
                );

                emit_fmem_i32(
                    code,
                    static_cast<uint32_t>(src),
                    simm,
                    mask,
                    zero,
                    static_cast<uint32_t>(lane)
                );

                local_get(code, 37);

                emit_call(code, 5);

                local_set(
                    code,
                    f_local(1, fdst, lane)
                );
            }
        }


        /*
         * --------------------------------------------------------
         * FSQRT_R
         * --------------------------------------------------------
         */
        else if (op < ceil_FSQRT_R) {

            emit_fp_sqrt(
                code,
                6,
                f_local(1, fdst, 0),
                37
            );

            local_set(
                code,
                f_local(1, fdst, 0)
            );

            emit_fp_sqrt(
                code,
                6,
                f_local(1, fdst, 1),
                37
            );

            local_set(
                code,
                f_local(1, fdst, 1)
            );
        }


        /*
         * --------------------------------------------------------
         * CBRANCH
         *
         * Still unsupported in this straight-line JIT.
         * --------------------------------------------------------
         */
        else if (op < ceil_CBRANCH) {

            return false;
        }


        /*
         * --------------------------------------------------------
         * CFROUND
         *
         * RandomX v2 semantics:
         *
         *     tmp = ROTR64(src, imm32 & 63)
         *
         *     if ((tmp & 0x3c) == 0)
         *         fprc = tmp & 3
         *
         * Important:
         *
         * - src is the RandomX integer source register
         * - imm32 is the rotate amount
         * - fprc is NOT simply imm32
         * - fprc remains unchanged when bits 2..5 are non-zero
         *
         * WASM:
         *
         *     0x8a = i64.rotr
         *     0x83 = i64.and
         *     0x51 = i64.eq
         *     0x04 = if
         *     0x40 = empty block type
         *     0xa7 = i32.wrap_i64
         *     0x0b = end
         * --------------------------------------------------------
         */
        else if (op < ceil_CFROUND) {

            const uint32_t rotate =
                ins.getImm32() & 63u;


            /*
             * Build:
             *
             *   (ROTR64(src, rotate) & 0x3c) == 0
             */
            local_get(
                code,
                5 + src
            );

            i64_const(
                code,
                static_cast<int64_t>(rotate)
            );

            // i64.rotr
            c.push_back(0x8a);

            // & 0x3c
            i64_const(
                code,
                0x3c
            );

            // i64.and
            c.push_back(0x83);

            // == 0
            i64_const(
                code,
                0
            );

            // i64.eq
            c.push_back(0x51);


            /*
             * if (...)
             */
            c.push_back(0x04);

            // block type = empty
            c.push_back(0x40);


            /*
             * fprc =
             *
             *     ROTR64(src, rotate) & 3
             */
            local_get(
                code,
                5 + src
            );

            i64_const(
                code,
                static_cast<int64_t>(rotate)
            );

            // i64.rotr
            c.push_back(0x8a);

            i64_const(
                code,
                3
            );

            // i64.and
            c.push_back(0x83);

            // i32.wrap_i64
            c.push_back(0xa7);

            // fprc local = 37
            local_set(code, 37);


            /*
             * end
             */
            c.push_back(0x0b);
        }


        /*
         * --------------------------------------------------------
         * ISTORE
         * --------------------------------------------------------
         */
        else if (op < ceil_ISTORE) {

            const uint32_t mask =
                (ins.getModCond() >= StoreL3Condition)
                    ? ScratchpadL3Mask
                    : (ins.getModMem()
                        ? ScratchpadL1Mask
                        : ScratchpadL2Mask);

            emit_address(
                code,
                static_cast<uint32_t>(dst),
                simm,
                mask,
                false
            );

            local_get(
                code,
                5 + src
            );

            i64_store(code);
        }


        /*
         * --------------------------------------------------------
         * NOP
         * --------------------------------------------------------
         */
        else if (op < ceil_NOP) {
            // Nothing to emit.
        }
    }


    /*
     * ------------------------------------------------------------
     * Store integer registers
     * ------------------------------------------------------------
     */
    for (int r = 0;
         r < RegistersCount;
         ++r) {

        local_get(
            code,
            static_cast<uint32_t>(5 + r)
        );

        store_reg(
            code,
            static_cast<uint32_t>(r)
        );
    }


    /*
     * ------------------------------------------------------------
     * Store floating-point registers
     * ------------------------------------------------------------
     */
    for (int r = 0;
         r < RegisterCountFlt;
         ++r) {

        for (int lane = 0;
             lane < 2;
             ++lane) {

            // F
            local_get(
                code,
                f_local(0, r, lane)
            );

            store_fp_lane(
                code,
                1,
                static_cast<uint32_t>(r),
                static_cast<uint32_t>(lane)
            );

            // E
            local_get(
                code,
                f_local(1, r, lane)
            );

            store_fp_lane(
                code,
                2,
                static_cast<uint32_t>(r),
                static_cast<uint32_t>(lane)
            );

            // A
            local_get(
                code,
                f_local(2, r, lane)
            );

            store_fp_lane(
                code,
                3,
                static_cast<uint32_t>(r),
                static_cast<uint32_t>(lane)
            );
        }
    }


    /*
     * End of rx_jit function.
     */
    code.push_back(0x0b);


    /*
     * ------------------------------------------------------------
     * WASM MODULE HEADER
     * ------------------------------------------------------------
     */
    module_.insert(
        module_.end(),
        {
            0x00, 0x61, 0x73, 0x6d,
            0x01, 0x00, 0x00, 0x00
        }
    );


    /*
     * ------------------------------------------------------------
     * TYPE SECTION
     * ------------------------------------------------------------
     *
     * 0 = rx_jit
     * 1 = mulh_u64(i64,i64) -> i64
     * 2 = fp_bin(i64,i64,i32) -> i64
     * 3 = fp_sqrt(i64,i32) -> i64
     * 4 = fp_from_i32(i32) -> i64
     */
    std::vector<uint8_t> type;

    uleb(type, 5);


    /*
     * Type 0:
     *
     * rx_jit(
     *     i32,
     *     i32,
     *     i32,
     *     i32,
     *     i32
     * ) -> void
     */
    type.push_back(0x60);
    uleb(type, 5);

    for (int i = 0; i < 5; ++i)
        type.push_back(0x7f);

    uleb(type, 0);


    /*
     * Type 1:
     *
     * (i64, i64) -> i64
     */
    type.push_back(0x60);
    uleb(type, 2);
    type.push_back(0x7e);
    type.push_back(0x7e);
    uleb(type, 1);
    type.push_back(0x7e);


    /*
     * Type 2:
     *
     * (i64, i64, i32) -> i64
     */
    type.push_back(0x60);
    uleb(type, 3);
    type.push_back(0x7e);
    type.push_back(0x7e);
    type.push_back(0x7f);
    uleb(type, 1);
    type.push_back(0x7e);


    /*
     * Type 3:
     *
     * (i64, i32) -> i64
     */
    type.push_back(0x60);
    uleb(type, 2);
    type.push_back(0x7e);
    type.push_back(0x7f);
    uleb(type, 1);
    type.push_back(0x7e);


    /*
     * Type 4:
     *
     * (i32) -> i64
     */
    type.push_back(0x60);
    uleb(type, 1);
    type.push_back(0x7f);
    uleb(type, 1);
    type.push_back(0x7e);


    section(
        module_,
        1,
        type
    );


    /*
     * ------------------------------------------------------------
     * IMPORT SECTION
     * ------------------------------------------------------------
     */
    std::vector<uint8_t> imports;

    uleb(imports, 9);


    // 0: mulh_u64
    bytes(imports, "env", 3);
    bytes(imports, "mulh_u64", 8);
    imports.push_back(0x00);
    uleb(imports, 1);


    // 1: mulh_s64
    bytes(imports, "env", 3);
    bytes(imports, "mulh_s64", 8);
    imports.push_back(0x00);
    uleb(imports, 1);


    // 2: fp_add
    bytes(imports, "env", 3);
    bytes(imports, "fp_add", 6);
    imports.push_back(0x00);
    uleb(imports, 2);


    // 3: fp_sub
    bytes(imports, "env", 3);
    bytes(imports, "fp_sub", 6);
    imports.push_back(0x00);
    uleb(imports, 2);


    // 4: fp_mul
    bytes(imports, "env", 3);
    bytes(imports, "fp_mul", 6);
    imports.push_back(0x00);
    uleb(imports, 2);


    // 5: fp_div
    bytes(imports, "env", 3);
    bytes(imports, "fp_div", 6);
    imports.push_back(0x00);
    uleb(imports, 2);


    // 6: fp_sqrt
    bytes(imports, "env", 3);
    bytes(imports, "fp_sqrt", 7);
    imports.push_back(0x00);
    uleb(imports, 3);


    // 7: fp_from_i32
    bytes(imports, "env", 3);
    bytes(imports, "fp_from_i32", 11);
    imports.push_back(0x00);
    uleb(imports, 4);


    // 8: memory
    bytes(imports, "env", 3);
    bytes(imports, "memory", 6);
    imports.push_back(0x02);
    imports.push_back(0x00);
    uleb(imports, 1);


    section(
        module_,
        2,
        imports
    );


    /*
     * ------------------------------------------------------------
     * FUNCTION SECTION
     * ------------------------------------------------------------
     *
     * One local function:
     *
     *     type 0 = rx_jit
     */
    std::vector<uint8_t> funcs;

    uleb(funcs, 1);
    uleb(funcs, 0);

    section(
        module_,
        3,
        funcs
    );


    /*
     * ------------------------------------------------------------
     * EXPORT SECTION
     * ------------------------------------------------------------
     *
     * Function index:
     *
     * Imports:
     *   0..7 = functions
     *
     *   8 = local rx_jit
     */
    std::vector<uint8_t> exports;

    uleb(exports, 1);

    bytes(
        exports,
        "rx_jit",
        6
    );

    exports.push_back(0x00);

    uleb(exports, 8);

    section(
        module_,
        7,
        exports
    );


    /*
     * ------------------------------------------------------------
     * CODE SECTION
     * ------------------------------------------------------------
     *
     * Locals:
     *
     *   8  x i64 = integer registers
     *   24 x i64 = F/E/A
     *   1  x i32 = fprc
     */
    std::vector<uint8_t> body;

    uleb(body, 3);


    // 8 x i64
    uleb(body, 8);
    body.push_back(0x7e);


    // 24 x i64
    uleb(body, 24);
    body.push_back(0x7e);


    // 1 x i32
    uleb(body, 1);
    body.push_back(0x7f);


    /*
     * Function instructions.
     */
    body.insert(
        body.end(),
        code.begin(),
        code.end()
    );


    std::vector<uint8_t> codes;

    uleb(codes, 1);

    uleb(
        codes,
        static_cast<uint32_t>(
            body.size()
        )
    );

    codes.insert(
        codes.end(),
        body.begin(),
        body.end()
    );


    section(
        module_,
        10,
        codes
    );


    return true;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
