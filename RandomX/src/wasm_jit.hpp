#pragma once

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "program.hpp"

namespace randomx {

// Stage 6 WASM JIT runtime bridge.
// The generated module operates directly on the RandomX register file:
//   rx_jit(i32 regs, i32 f, i32 e, i32 a, i32 scratchpad)
// Integer registers are 8 x uint64. Each FP register is 16 bytes
// (two packed f64 lanes).
class WasmJit {
public:
    WasmJit() = default;

    bool compile(const Program& program);

    bool execute(uint8_t* regs,
                 uint8_t* f,
                 uint8_t* e,
                 uint8_t* a,
                 uint8_t* scratchpad);

    const std::vector<uint8_t>& module() const { return module_; }

    void clear() { module_.clear(); }

private:
    std::vector<uint8_t> module_;
};

} // namespace randomx

#endif // __EMSCRIPTEN__
