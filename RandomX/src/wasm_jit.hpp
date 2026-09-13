#pragma once

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "program.hpp"

namespace randomx {

// Stage 2 WASM JIT emitter.
// Emits executable WASM for the integer RandomX instruction subset and the
// scratchpad load/store forms. Floating point, high multiply, branches and
// CFROUND remain interpreter-only until their semantics are validated.
class WasmJit {
public:
    WasmJit() = default;

    // Builds a standalone module importing the host linear memory as
    // env.memory and exporting:
    //   rx_jit(i32 regs_ptr, i32 scratchpad_ptr)
    //
    // regs_ptr points to 8 uint64 integer registers.
    // scratchpad_ptr points to the RandomX scratchpad in the same linear memory.
    bool compile(const Program& program);

    const std::vector<uint8_t>& module() const { return module_; }

    void clear() { module_.clear(); }

private:
    std::vector<uint8_t> module_;
};

} // namespace randomx

#endif // __EMSCRIPTEN__
