#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <emscripten/emscripten.h>

namespace randomx {

namespace {

EM_JS(int, randomx_wasm_execute_module, (const uint8_t* module_ptr,
                                         int module_size,
                                         int regs_ptr,
                                         int scratchpad_ptr), {
    try {
        const bytes = HEAPU8.slice(module_ptr, module_ptr + module_size);
        const key = String(module_ptr) + ':' + String(module_size);
        const cache = Module.__randomxJitCache || (Module.__randomxJitCache = Object.create(null));
        let fn = cache[key];

        if (!fn) {
            const wasmModule = new WebAssembly.Module(bytes);
            const memory = Module['wasmMemory'];
            if (!memory) {
                console.error('RandomX WASM JIT: wasmMemory unavailable');
                return 0;
            }

            const instance = new WebAssembly.Instance(wasmModule, {
                env: { memory: memory }
            });

            fn = instance.exports.rx_jit;
            if (typeof fn !== 'function') {
                console.error('RandomX WASM JIT: rx_jit export missing');
                return 0;
            }
            cache[key] = fn;
        }

        fn(regs_ptr, scratchpad_ptr);
        return 1;
    } catch (e) {
        console.error('RandomX WASM JIT runtime error:', e);
        return 0;
    }
});

} // namespace

bool WasmJit::execute(uint8_t* regs, uint8_t* scratchpad) {
    if (module_.empty() || regs == nullptr || scratchpad == nullptr) {
        return false;
    }

    return randomx_wasm_execute_module(
        module_.data(),
        static_cast<int>(module_.size()),
        static_cast<int>(reinterpret_cast<uintptr_t>(regs)),
        static_cast<int>(reinterpret_cast<uintptr_t>(scratchpad))) != 0;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
