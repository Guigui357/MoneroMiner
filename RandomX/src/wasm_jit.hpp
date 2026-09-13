#pragma once

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "program.hpp"

namespace randomx {

// Stage 3 WASM JIT runtime bridge.
// The emitter builds executable WASM for the validated integer subset and
// this class can now execute that module against Emscripten linear memory.
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

    // Instantiates/caches the generated module and executes rx_jit.
    // Both pointers must refer to Emscripten linear memory.
    bool execute(uint8_t* regs, uint8_t* scratchpad);

    const std::vector<uint8_t>& module() const { return module_; }

    void clear() { module_.clear(); }

private:
    std::vector<uint8_t> module_;
};

} // namespace randomx

#endif // __EMSCRIPTEN__
