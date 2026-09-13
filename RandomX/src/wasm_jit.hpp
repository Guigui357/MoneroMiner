#pragma once

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "program.hpp"

namespace randomx {

// Stage 3 WASM JIT runtime.
// The emitter produces a standalone module and the runtime synchronously
// compiles/instantiates that module through the browser's WebAssembly API.
class WasmJit {
public:
    WasmJit() = default;

    // Builds a standalone module importing env.memory and exporting:
    //   rx_jit(i32 regs_ptr, i32 scratchpad_ptr)
    bool compile(const Program& program);

    // Instantiates the generated module and executes rx_jit against the
    // current Emscripten linear memory.
    bool execute(uint8_t* regs, uint8_t* scratchpad);

    const std::vector<uint8_t>& module() const { return module_; }

    void clear() { module_.clear(); }

private:
    std::vector<uint8_t> module_;
};

} // namespace randomx

#endif // __EMSCRIPTEN__
