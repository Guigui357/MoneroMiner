#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <emscripten/emscripten.h>

namespace randomx {
namespace {

EM_JS(int, randomx_wasm_execute_module, (const uint8_t* module_ptr,
                                         int module_size,
                                         int regs_ptr,
                                         int f_ptr,
                                         int e_ptr,
                                         int a_ptr,
                                         int scratchpad_ptr), {
    try {
        const bytes = HEAPU8.slice(module_ptr, module_ptr + module_size);
        const key = String(module_ptr) + ':' + String(module_size);
        const cache = Module.__randomxJitCache || (Module.__randomxJitCache = Object.create(null));
        const stats = Module.__randomxJitStats || (Module.__randomxJitStats = {executions: 0});
        let fn = cache[key];

        if (!fn) {
            console.log('[WASM-JIT] Compilando WebAssembly.Module, bytes=' + module_size);
            const wasmModule = new WebAssembly.Module(bytes);
            console.log('[WASM-JIT] WebAssembly.Module = OK');

            const memory = Module['wasmMemory'];
            if (!memory) {
                console.error('[WASM-JIT] wasmMemory indisponivel');
                return 0;
            }

            /* Native-WASM integer high multiply helper. */
            let mulh = Module.__randomxMulhHelper;
            if (!mulh) {
                const helperBytes = Uint8Array.from(atob(
                    'AGFzbQEAAAABBwFgAn5+AX4DAwIAAAcXAghtdWxoX3U2NAAACG11bGhfczY0AAEKngECbwEIfiAAQv////8PgyECIABCIIghAyABQv////8PgyEEIAFCIIghBSACIAR+IQYgAyAEfiEHIAIgBX4hCCAGQiCIIAdC/////w+DfCAIQv////8Pg3whCSAHQiCIIAhCIIh8IAlCIIh8IAMgBX58CywBAn4gACABEAAhAiAAQj+HIAGDIQMgAiADfSECIAFCP4cgAIMhAyACIAN9Cw=='
                ), c => c.charCodeAt(0));
                const helperModule = new WebAssembly.Module(helperBytes);
                const helperInstance = new WebAssembly.Instance(helperModule, {});
                mulh = helperInstance.exports;
                Module.__randomxMulhHelper = mulh;
                console.log('[WASM-JIT] native WASM mulh helper = ACTIVE');
            }

            /*
             * Native-WASM IEEE-754 arithmetic helper.
             *
             * The generated JIT passes the exact binary64 bit pattern as i64.
             * The helper reinterprets it as f64, executes the hardware/WASM
             * operation, and returns the resulting binary64 bit pattern.
             *
             * Directed RandomX rounding is still handled below, so this
             * optimization does not silently replace RandomX's fprc semantics.
             */
            let fp = Module.__randomxFpHelper;
            if (!fp) {
                const fpHelperBytes = Uint8Array.from(atob(
                    'AGFzbQEAAAABEwNgA35+fwF+YAJ+fwF+YAF/AX4DBwYAAAAAAQIHPQYGZnBfYWRkAAAGZnBfc3ViAAEGZnBfbXVsAAIGZnBfZGl2AAMHZnBfc3FydAAEC2ZwX2Zyb21faTMyAAUKPAYKACAAvyABv6C9CwoAIAC/IAG/ob0LCgAgAL8gAb+ivQsKACAAvyABv6O9CwcAIAC/n70LBgAgALe9Cw=='
                ), c => c.charCodeAt(0));
                const fpHelperModule = new WebAssembly.Module(fpHelperBytes);
                const fpHelperInstance = new WebAssembly.Instance(fpHelperModule, {});
                fp = fpHelperInstance.exports;
                Module.__randomxFpHelper = fp;
                console.log('[WASM-JIT] native WASM FP helper = ACTIVE');
            }

            const fpBits = new DataView(new ArrayBuffer(8));
            const bitsToF64 = (bits) => {
                fpBits.setBigUint64(0, BigInt.asUintN(64, bits), true);
                return fpBits.getFloat64(0, true);
            };
            const f64ToBits = (value) => {
                fpBits.setFloat64(0, value, true);
                return fpBits.getBigUint64(0, true);
            };
            const ordered = (bits) => {
                bits = BigInt.asUintN(64, bits);
                return (bits >> 63n) ? (~bits & ((1n << 64n) - 1n)) : (bits | (1n << 63n));
            };
            const fromOrdered = (x) => {
                const sign = (x >> 63n) & 1n;
                return sign ? (x & ((1n << 63n) - 1n)) : (~x & ((1n << 64n) - 1n));
            };
            const nextBits = (bits, direction) => {
                const v = bitsToF64(bits);
                if (Number.isNaN(v)) return bits;
                if (v === 0 && direction < 0) return 0x8000000000000000n;
                if (v === 0 && direction > 0) return 0x0000000000000001n;
                let o = ordered(bits);
                o += direction > 0 ? 1n : -1n;
                return fromOrdered(o);
            };
            const exact = (bits) => {
                bits = BigInt.asUintN(64, bits);
                const sign = (bits >> 63n) ? -1n : 1n;
                const rawExp = Number((bits >> 52n) & 0x7ffn);
                const frac = bits & 0xfffffffffffffn;
                if (rawExp === 0x7ff) return null;
                const mant = rawExp === 0 ? frac : (frac | (1n << 52n));
                const exp = rawExp === 0 ? -1074 : rawExp - 1075;
                return {n: sign * mant, e: exp};
            };
            const cmpRational = (a, b) => {
                if (a.n === 0n && b.n === 0n) return 0;
                if (a.n < 0n && b.n >= 0n) return -1;
                if (a.n >= 0n && b.n < 0n) return 1;
                const an = a.n < 0n ? -a.n : a.n;
                const bn = b.n < 0n ? -b.n : b.n;
                const e = Math.min(a.e, b.e);
                const x = an << BigInt(a.e - e);
                const y = bn << BigInt(b.e - e);
                if (x === y) return 0;
                const r = x < y ? -1 : 1;
                return a.n < 0n ? -r : r;
            };
            const roundDirected = (aBits, bBits, nearestBits, mode, op) => {
                if (mode === 0) return nearestBits;
                const da = exact(aBits), db = exact(bBits);
                if (!da || !db) return nearestBits;
                let target;
                if (op === 0 || op === 1) {
                    const e = Math.min(da.e, db.e);
                    const an = da.n << BigInt(da.e - e);
                    const bn = db.n << BigInt(db.e - e);
                    target = {n: op === 0 ? an + bn : an - bn, e};
                } else if (op === 2) {
                    target = {n: da.n * db.n, e: da.e + db.e};
                } else {
                    return nearestBits;
                }
                const cand = exact(nearestBits);
                if (!cand) return nearestBits;
                const cmp = cmpRational(cand, target);
                let adjust = 0;
                if (mode === 1 && cmp > 0) adjust = -1;
                if (mode === 2 && cmp < 0) adjust = 1;
                if (mode === 3) {
                    if (target.n >= 0n && cmp > 0) adjust = -1;
                    if (target.n < 0n && cmp < 0) adjust = 1;
                }
                return adjust ? nextBits(nearestBits, adjust) : nearestBits;
            };

            const fpBin = (aBits, bBits, mode, op) => {
                let nearestBits;
                if (op === 0) nearestBits = fp.fp_add(aBits, bBits, 0);
                else if (op === 1) nearestBits = fp.fp_sub(aBits, bBits, 0);
                else if (op === 2) nearestBits = fp.fp_mul(aBits, bBits, 0);
                else nearestBits = fp.fp_div(aBits, bBits, 0);
                return roundDirected(aBits, bBits, nearestBits, mode | 0, op);
            };
            const fpSqrt = (aBits, mode) => fp.fp_sqrt(aBits, mode | 0);
            const fpFromI32 = (value) => fp.fp_from_i32(value | 0);

            const instance = new WebAssembly.Instance(wasmModule, {
                env: {
                    memory: memory,
                    mulh_u64: mulh.mulh_u64,
                    mulh_s64: mulh.mulh_s64,
                    fp_add: (a, b, mode) => fpBin(a, b, mode, 0),
                    fp_sub: (a, b, mode) => fpBin(a, b, mode, 1),
                    fp_mul: (a, b, mode) => fpBin(a, b, mode, 2),
                    fp_div: (a, b, mode) => fpBin(a, b, mode, 3),
                    fp_sqrt: (a, mode) => fpSqrt(a, mode),
                    fp_from_i32: fpFromI32
                }
            });

            fn = instance.exports.rx_jit;
            if (typeof fn !== 'function') {
                console.error('[WASM-JIT] rx_jit export ausente');
                return 0;
            }

            cache[key] = fn;
            console.log('[WASM-JIT] WebAssembly.Instance = OK; rx_jit = ACTIVE');
        }

        try {
            fn(regs_ptr, f_ptr, e_ptr, a_ptr, scratchpad_ptr);
        } catch (e) {
            console.error('[WASM-JIT] RX_JIT_TRAP:', e);
            console.error('[WASM-JIT] RX_JIT_TRAP_NAME:', e && e.name);
            console.error('[WASM-JIT] RX_JIT_TRAP_MESSAGE:', e && e.message);
            console.error('[WASM-JIT] RX_JIT_TRAP_STACK:', e && e.stack);
            return 0;
        }

        stats.executions++;
        if (stats.executions === 1 || (stats.executions % 10000) === 0) {
            console.log('[WASM-JIT] execucoes=' + stats.executions);
        }
        return 1;
    } catch (e) {
        console.error('[WASM-JIT] runtime error:', e);
        return 0;
    }
});

} // namespace

bool WasmJit::execute(uint8_t* regs,
                      uint8_t* f,
                      uint8_t* e,
                      uint8_t* a,
                      uint8_t* scratchpad) {
    if (module_.empty() || regs == nullptr || f == nullptr || e == nullptr ||
        a == nullptr || scratchpad == nullptr) {
        return false;
    }

    return randomx_wasm_execute_module(
        module_.data(),
        static_cast<int>(module_.size()),
        static_cast<int>(reinterpret_cast<uintptr_t>(regs)),
        static_cast<int>(reinterpret_cast<uintptr_t>(f)),
        static_cast<int>(reinterpret_cast<uintptr_t>(e)),
        static_cast<int>(reinterpret_cast<uintptr_t>(a)),
        static_cast<int>(reinterpret_cast<uintptr_t>(scratchpad))) != 0;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
