#pragma once

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstdint>
#include <vector>

namespace randomx {

// Stage 1 WASM JIT emitter.
// This deliberately starts with the integer register subset. Floating-point,
// scratchpad, CFROUND and superscalar handling remain in the interpreter until
// their WASM semantics are added and validated against the RandomX vectors.
class WasmJit {
public:
    WasmJit() = default;

    // Emit a minimal standalone WASM module containing an exported function
    // that executes the supplied integer micro-program. The function accepts
    // a pointer to a 16-element uint64 register file and operates in-place.
    bool compile(const uint8_t* program, std::size_t size);

    const std::vector<uint8_t>& module() const { return module_; }

    void clear() { module_.clear(); }

private:
    std::vector<uint8_t> module_;
};

} // namespace randomx

#endif // __EMSCRIPTEN__
