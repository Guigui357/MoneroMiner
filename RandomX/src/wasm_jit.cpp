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
    local_set(c, 39);
    local_get(c, 0);

    i32_const(
        c,
        static_cast<int32_t>(r * 8)
    );

    c.push_back(0x6a); // i32.add
    local_get(c, 39);
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
//   38     = pc
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
    local_set(c, 39);
    local_get(c, ptr_local);

    i32_const(
        c,
        static_cast<int32_t>(
            index * 16 + lane * 8
        )
    );

    c.push_back(0x6a); // i32.add
    local_get(c, 39);
    i64_store(c);
}

static int f_local(int group,
                   int reg,
                   int lane) {
    return 13 +
           group * 16 +
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

static bool validate_opcode(uint32_t op,
                            uint32_t) {
    return op <= 255u;
}

static bool supported(const Instruction& ins) {
    const uint32_t op =
        static_cast<uint32_t>(ins.opcode);

    return op <= 255u;
}

} // namespace


bool WasmJit::compile(const Program& program) {
    module_.clear();

    const uint32_t program_size =
        static_cast<uint32_t>(program.getSize());

    /*
     * ------------------------------------------------------------
     * PASS 1: validate all RandomX instructions
     * ------------------------------------------------------------
     */
    for (uint32_t pc = 0;
         pc < program_size;
         ++pc) {

        const Instruction& ins =
            program(static_cast<int>(pc));

        const uint32_t op =
            static_cast<uint32_t>(ins.opcode);

        if (!validate_opcode(op, pc))
            return false;

        if (!supported(ins)) {
            return false;
        }
    }

    /*
     * ------------------------------------------------------------
     * PASS 2: calculate the real RandomX CBRANCH targets.
     *
     * RandomX does not store an explicit target in CBRANCH.
     *
     * The target is:
     *
     *     instruction after the last instruction that modified
     *     the destination integer register.
     *
     * If the register was never modified:
     *
     *     target = 0
     *
     * CBRANCH then marks every integer register as modified at
     * the current instruction.
     * ------------------------------------------------------------
     */
    std::vector<uint32_t> branch_target(
        program_size,
        0
    );

    uint32_t last_modified[RegistersCount];

    for (uint32_t r = 0;
         r < RegistersCount;
         ++r) {
        last_modified[r] = UINT32_MAX;
    }

    for (uint32_t pc = 0;
         pc < program_size;
         ++pc) {

        const Instruction& ins =
            program(static_cast<int>(pc));

        const uint32_t op =
            static_cast<uint32_t>(ins.opcode);

        const uint32_t dst =
            static_cast<uint32_t>(reg(ins.dst));

        const uint32_t src =
            static_cast<uint32_t>(reg(ins.src));

        /*
         * IADD_RS through ISMULH_M all modify dst.
         */
        if (op < ceil_IMUL_RCP) {

            last_modified[dst] = pc;
        }

        /*
         * IMUL_RCP only changes the register when the divisor
         * requires an actual reciprocal multiplication.
         */
        else if (op < ceil_INEG_R) {

            const uint32_t divisor =
                ins.getImm32();

            if (!isZeroOrPowerOf2(divisor)) {
                last_modified[dst] = pc;
            }
        }

        /*
         * INEG_R through IROL_R modify dst.
         */
        else if (op < ceil_ISWAP_R) {

            last_modified[dst] = pc;
        }

        /*
         * ISWAP_R modifies both registers when they differ.
         */
        else if (op < ceil_FSWAP_R) {

            if (dst != src) {
                last_modified[dst] = pc;
                last_modified[src] = pc;
            }
        }

        /*
         * FSWAP_R through FSQRT_R do not modify integer
         * register usage.
         */
        else if (op < ceil_CBRANCH) {
            // Nothing.
        }

        /*
         * CBRANCH target calculation.
         */
        else if (op < ceil_CFROUND) {

            if (last_modified[dst] == UINT32_MAX) {
                branch_target[pc] = 0;
            } else {
                branch_target[pc] =
                    last_modified[dst] + 1;
            }

            /*
             * Official RandomX behavior:
             *
             * CBRANCH invalidates the previous modification
             * information for every integer register.
             */
            for (uint32_t r = 0;
                 r < RegistersCount;
                 ++r) {
                last_modified[r] = pc;
            }
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
     * Initialize fprc.
     * ------------------------------------------------------------
     */
    i32_const(code, 0);
    local_set(code, 37);

    /*
     * ------------------------------------------------------------
     * Initialize program counter.
     * ------------------------------------------------------------
     */
    i32_const(code, 0);
    local_set(code, 39);

    /*
     * ------------------------------------------------------------
     * RANDOMX THREADED WASM DISPATCHER
     *
     * Uses a WASM br_table instead of:
     *
     *     if (pc == 0)
     *     if (pc == 1)
     *     if (pc == 2)
     *     ...
     *
     * Structure:
     *
     *   block $done
     *     loop $dispatch
     *
     *       block $caseN
     *         ...
     *           block $case1
     *             block $case0
     *               local.get $pc
     *               br_table $case0 ... $caseN $done
     *
     *             instruction 0
     *             br $dispatch
     *
     *           instruction 1
     *           br $dispatch
     *
     *         ...
     *
     *       instruction N
     *       br $dispatch
     *
     *     end
     *   end
     *
     * case i is selected directly by the value of pc.
     * ------------------------------------------------------------
     */

    /*
     * $done
     */
    code.push_back(0x02);
    code.push_back(0x40);

    /*
     * $dispatch
     */
    code.push_back(0x03);
    code.push_back(0x40);

    /*
     * ------------------------------------------------------------
     * Create N nested case blocks.
     *
     * The innermost block is case 0.
     *
     * Therefore:
     *
     *   case 0 -> branch depth 0
     *   case 1 -> branch depth 1
     *   ...
     *   case N -> branch depth N
     *
     * The default target is $done.
     * ------------------------------------------------------------
     */
    for (uint32_t i = 0;
         i < program_size;
         ++i) {

        code.push_back(0x02);
        code.push_back(0x40);
    }

    /*
     * ------------------------------------------------------------
     * pc -> br_table
     * ------------------------------------------------------------
     */
    local_get(code, 38);

    /*
     * br_table
     */
    code.push_back(0x0e);

    /*
     * Number of explicit targets.
     */
    uleb(code, program_size);

    /*
     * case 0 ... case N
     */
    for (uint32_t i = 0;
         i < program_size;
         ++i) {

        /*
         * Because case blocks are nested:
         *
         * case 0 = depth 0
         * case 1 = depth 1
         * ...
         */
        uleb(code, i);
    }

    /*
     * ------------------------------------------------------------
     * Default target = $done
     *
     * Current nesting:
     *
     *   case0      depth 0
     *   case1      depth 1
     *   ...
     *   caseN      depth N
     *   dispatch   depth N+1
     *   done       depth N+2
     * ------------------------------------------------------------
     */
    uleb(code, program_size + 1);

    /*
     * ------------------------------------------------------------
     * Emit instruction bodies.
     *
     * IMPORTANT:
     *
     * The blocks are closed one at a time before the next
     * instruction body.
     *
     * Therefore:
     *
     *   end case0
     *   instruction 0
     *
     *   end case1
     *   instruction 1
     *
     *   ...
     * ------------------------------------------------------------
     */
    for (uint32_t pc = 0;
         pc < program_size;
         ++pc) {

        const Instruction& ins =
            program(static_cast<int>(pc));

        const int op =
            static_cast<int>(ins.opcode);

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
         * Close the current case block.
         *
         * For pc=0 this closes case0.
         * For pc=1 this closes case1.
         * etc.
         * --------------------------------------------------------
         */
        code.push_back(0x0b);

        /*
         * --------------------------------------------------------
         * REAL RANDOMX INSTRUCTION
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

            code.push_back(0x86); // i64.shl

            if (dst == RegisterNeedsDisplacement) {
                i64_const(code, simm);
                code.push_back(0x7c); // i64.add
            }

            code.push_back(0x7c); // i64.add

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IADD_M) {

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

            code.push_back(0x7c);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_ISUB_R) {

            local_get(code, 5 + dst);

            if (src == dst)
                i64_const(code, simm);
            else
                local_get(code, 5 + src);

            code.push_back(0x7d);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_ISUB_M) {

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

            code.push_back(0x7d);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IMUL_R) {

            local_get(code, 5 + dst);

            if (src == dst)
                i64_const(code, simm);
            else
                local_get(code, 5 + src);

            code.push_back(0x7e);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IMUL_M) {

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

            code.push_back(0x7e);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IMULH_R) {

            local_get(code, 5 + dst);
            local_get(code, 5 + src);

            emit_call(code, 0);

            local_set(code, 5 + dst);
        }

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

            local_set(code, 5 + dst);
        }

        else if (op < ceil_ISMULH_R) {

            local_get(code, 5 + dst);
            local_get(code, 5 + src);

            emit_call(code, 1);

            local_set(code, 5 + dst);
        }

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

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IMUL_RCP) {

            const uint32_t divisor =
                ins.getImm32();

            if (!isZeroOrPowerOf2(divisor)) {

                local_get(code, 5 + dst);

                i64_const(
                    code,
                    static_cast<int64_t>(
                        randomx_reciprocal(divisor)
                    )
                );

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

            if (src == dst)
                i64_const(code, simm);
            else
                local_get(code, 5 + src);

            code.push_back(0x85);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IXOR_M) {

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

            code.push_back(0x85);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IROR_R) {

            local_get(code, 5 + dst);

            if (src == dst)
                i64_const(
                    code,
                    static_cast<int64_t>(
                        ins.getImm32()
                    )
                );
            else
                local_get(code, 5 + src);

            code.push_back(0x88);

            local_set(code, 5 + dst);
        }

        else if (op < ceil_IROL_R) {

            local_get(code, 5 + dst);

            if (src == dst)
                i64_const(
                    code,
                    static_cast<int64_t>(
                        ins.getImm32()
                    )
                );
            else
                local_get(code, 5 + src);

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

        else if (op < ceil_FADD_R) {

            emit_fp_bin(
                code,
                2,
                f_local(0, fdst, 0),
                f_local(2, fsrc, 0),
                61
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
                61
            );

            local_set(
                code,
                f_local(0, fdst, 1)
            );
        }

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

        else if (op < ceil_FSUB_R) {

            emit_fp_bin(
                code,
                3,
                f_local(0, fdst, 0),
                f_local(2, fsrc, 0),
                61
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
                61
            );

            local_set(
                code,
                f_local(0, fdst, 1)
            );
        }

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

                local_get(code, 61);

                emit_call(code, 3);

                local_set(
                    code,
                    f_local(0, fdst, lane)
                );
            }
        }

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

                code.push_back(0x85);

                local_set(
                    code,
                    f_local(0, fdst, lane)
                );
            }
        }

        else if (op < ceil_FMUL_R) {

            emit_fp_bin(
                code,
                4,
                f_local(1, fdst, 0),
                f_local(2, fsrc, 0),
                61
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
                61
            );

            local_set(
                code,
                f_local(1, fdst, 1)
            );
        }

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

                local_get(code, 61);

                emit_call(code, 5);

                local_set(
                    code,
                    f_local(1, fdst, lane)
                );
            }
        }

        else if (op < ceil_FSQRT_R) {

            emit_fp_sqrt(
                code,
                6,
                f_local(1, fdst, 0),
                61
            );

            local_set(
                code,
                f_local(1, fdst, 0)
            );

            emit_fp_sqrt(
                code,
                6,
                f_local(1, fdst, 1),
                61
            );

            local_set(
                code,
                f_local(1, fdst, 1)
            );
        }

        else if (op < ceil_CBRANCH) {

            const uint32_t shift =
                static_cast<uint32_t>(
                    ins.getModCond()
                ) +
                RANDOMX_JUMP_OFFSET;

            const uint64_t signed_imm =
                static_cast<uint64_t>(
                    static_cast<int64_t>(
                        static_cast<int32_t>(
                            ins.getImm32()
                        )
                    )
                );

            uint64_t cimm =
                signed_imm |
                (uint64_t(1) << shift);

            if (shift > 0) {
                cimm &=
                    ~(uint64_t(1) << (shift - 1));
            }

            const uint64_t condition_mask =
                ((uint64_t(1) << RANDOMX_JUMP_BITS) - 1ULL)
                << shift;

            /*
             * dst += cimm
             */
            local_get(code, 5 + dst);

            i64_const(
                code,
                static_cast<int64_t>(cimm)
            );

            code.push_back(0x7c);

            local_set(code, 5 + dst);

            /*
             * Default = pc + 1.
             */
            local_get(code, 62);

            i32_const(code, 1);

            code.push_back(0x6a);

            local_set(code, 62);

            /*
             * Test condition.
             */
            local_get(code, 5 + dst);

            i64_const(
                code,
                static_cast<int64_t>(
                    condition_mask
                )
            );

            code.push_back(0x83);

            i64_const(code, 0);

            code.push_back(0x51);

            /*
             * Taken branch.
             */
            code.push_back(0x04);
            code.push_back(0x40);

            i32_const(
                code,
                static_cast<int32_t>(
                    branch_target[pc]
                )
            );

            local_set(code, 62);

            code.push_back(0x0b);

            /*
             * Return directly to $dispatch.
             *
             * At this point:
             *
             *   case pc+1 ... case N
             *   loop
             *
             * are open, plus the CBRANCH if has already ended.
             *
             * Depth to loop:
             *
             *   program_size - pc
             */
            code.push_back(0x0c);

            uleb(
                code,
                program_size - pc - 1
            );

            /*
             * No generic pc++ and no second dispatch branch.
             */
            continue;
        }

        else if (op < ceil_CFROUND) {

            const uint32_t rotate =
                ins.getImm32() & 63u;

            local_get(code, 5 + src);

            i64_const(
                code,
                static_cast<int64_t>(rotate)
            );

            code.push_back(0x8a);

            i64_const(code, 0x3c);

            code.push_back(0x83);

            i64_const(code, 0);

            code.push_back(0x51);

            code.push_back(0x04);
            code.push_back(0x40);

            local_get(code, 5 + src);

            i64_const(
                code,
                static_cast<int64_t>(rotate)
            );

            code.push_back(0x8a);

            i64_const(code, 3);

            code.push_back(0x83);

            code.push_back(0xa7);

            local_set(code, 61);

            code.push_back(0x0b);
        }

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

            local_get(code, 5 + src);

            i64_store(code);
        }

        else if (op < ceil_NOP) {
            /*
             * NOP
             */
        }

        /*
         * --------------------------------------------------------
         * Generic sequential advance.
         *
         * CBRANCH used `continue`, so it never reaches here.
         * --------------------------------------------------------
         */
        local_get(code, 62);

        i32_const(code, 1);

        code.push_back(0x6a);

        local_set(code, 62);

        /*
         * --------------------------------------------------------
         * Return to $dispatch.
         *
         * At instruction pc:
         *
         *   case pc+1 ... case N = program_size - pc - 1
         *
         * plus:
         *
         *   loop
         *
         * Therefore:
         *
         *   depth = program_size - pc
         * --------------------------------------------------------
         */
        code.push_back(0x0c);

        uleb(
            code,
            program_size - pc
        );
    }

    /*
     * ------------------------------------------------------------
     * Close the remaining case block(s).
     *
     * The last instruction body is inside the outermost case
     * structure. We need to close the remaining nested blocks
     * before closing the dispatch loop.
     *
     * ------------------------------------------------------------
     */

    /*
     * Close $dispatch.
     */
    code.push_back(0x0b);

    /*
     * Close $done.
     */
    code.push_back(0x0b);
    
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
     *   1  x i32 = pc
     * ------------------------------------------------------------
     */
    std::vector<uint8_t> body;

    uleb(body, 4);

    // 8 x i64
    uleb(body, 8);
    body.push_back(0x7e);

    // 24 x i64
    uleb(body, 24);
    body.push_back(0x7e);

    // 1 x i32 = fprc
    uleb(body, 1);
    body.push_back(0x7f);

    // 1 x i32 = pc
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
