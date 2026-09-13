#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include <cstdint>

#include <emscripten.h>

namespace randomx {
namespace {

// Compile/instantiate once per generated module. The hash is only a cache key;
// the module bytes are still owned by WasmJit on the C++ side.
EM_JS(int, randomx_wasm_execute_module,
      (const uint8_t* module_ptr, int module_size,
       int regs_ptr, int scratchpad_ptr), {
    if (!Module.__randomxWasmJitCache) {
        Module.__randomxWasmJitCache = new Map();
    }

    const bytes = HEAPU8.slice(module_ptr, module_ptr + module_size);

    // FNV-1a gives a cheap stable cache key without copying the module into a
    // JavaScript string. A module collision would only affect the cache hit;
    // the size is included as an additional discriminator.
    let hash = 2166136261 >>> 0;
    for (let i = 0; i < bytes.length; ++i) {
        hash ^= bytes[i];
        hash = Math.imul(hash, 16777619) >>> 0;
    }
    const key = bytes.length + ':' + hash;

    let instance = Module.__randomxWasmJitCache.get(key);
    if (!instance) {
        try {
            const wasmModule = new WebAssembly.Module(bytes);
            const memory = Module['wasmMemory'];
            if (!memory) {
                console.error('RandomX WASM JIT: Emscripten wasmMemory unavailable');
                return 0;
            }

            instance = new WebAssembly.Instance(wasmModule, {
                env: { memory: memory }
            });

            if (!instance.exports || typeof instance.exports.rx_jit !== 'function') {
                console.error('RandomX WASM JIT: rx_jit export missing');
                return 0;
            }

            Module.__randomxWasmJitCache.set(key, instance);
        } catch (e) {
            console.error('RandomX WASM JIT: module instantiation failed', e);
            return 0;
        }
    }

    try {
        instance.exports.rx_jit(regs_ptr >>> 0, scratchpad_ptr >>> 0);
        return 1;
    } catch (e) {
        console.error('RandomX WASM JIT: execution failed', e);
        return 0;
    }
});

} // namespace

bool WasmJit::execute(uint8_t* regs, uint8_t* scratchpad) {
    if (module_.empty() || regs == nullptr || scratchpad == nullptr) {
        return false;
    }

    const uintptr_t regs_addr = reinterpret_cast<uintptr_t>(regs);
    const uintptr_t scratchpad_addr = reinterpret_cast<uintptr_t>(scratchpad);

    // Emscripten's wasm32 pointers are 32-bit. Keep the explicit check so a
    // future wasm64 build fails safely instead of truncating an address.
    if (regs_addr > 0xffffffffULL || scratchpad_addr > 0xffffffffULL ||
        module_.size() > static_cast<size_t>(0x7fffffffU)) {
        return false;
    }

    return randomx_wasm_execute_module(
               module_.data(), static_cast<int>(module_.size()),
               static_cast<int>(regs_addr), static_cast<int>(scratchpad_addr)) != 0;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
