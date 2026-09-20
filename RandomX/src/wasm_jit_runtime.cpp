#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <cstdio>
#include <climits>
#include <emscripten/emscripten.h>

namespace randomx {
namespace {

/*
 * ============================================================================
 * RandomX WASM JIT runtime
 * ============================================================================
 *
 * ABI:
 *
 *   rx_jit(
 *       i32 regs_ptr,
 *       i32 f_ptr,
 *       i32 e_ptr,
 *       i32 a_ptr,
 *       i32 scratchpad_ptr
 *   )
 *
 * Os ponteiros são offsets da memória linear WASM/Emscripten.
 *
 * Retorno do runtime:
 *
 *   1  = execução OK
 *   0  = falha
 *
 * O motivo detalhado fica em:
 *
 *   Module.__randomxLastJitError
 *
 * ============================================================================
 */

EM_JS(int, randomx_wasm_execute_module, (
    const uint8_t* module_ptr,
    int module_size,
    int regs_ptr,
    int f_ptr,
    int e_ptr,
    int a_ptr,
    int scratchpad_ptr
), {
    /*
     * Códigos internos de diagnóstico.
     *
     * O C++ recebe 1/0, enquanto o código detalhado fica no JS.
     */
    const ERR = {
        OK: 0,
        INVALID_ARGUMENTS: 1,
        MODULE_CREATE: 2,
        MEMORY: 3,
        MULH_HELPER: 4,
        FP_HELPER: 5,
        INSTANCE_CREATE: 6,
        EXPORT_MISSING: 7,
        EXECUTE_TRAP: 8,
        RUNTIME: 9,
        POINTER_OOB: 10
    };

    const setError = (code, message, exception) => {
        Module.__randomxLastJitError = {
            code: code,
            message: message,
            exception: exception || null
        };

        if (exception) {
            console.error(
                '[WASM-JIT] ERROR ' + code + ': ' + message,
                exception
            );
        } else {
            console.error(
                '[WASM-JIT] ERROR ' + code + ': ' + message
            );
        }
    };

    const clearError = () => {
        Module.__randomxLastJitError = {
            code: ERR.OK,
            message: 'OK',
            exception: null
        };
    };

    try {
        clearError();

        /*
         * --------------------------------------------------------------------
         * Normalização dos argumentos.
         *
         * Emscripten WASM32 usa ponteiros i32.
         * >>> 0 transforma corretamente em offsets unsigned.
         * --------------------------------------------------------------------
         */

        const moduleOffset = module_ptr >>> 0;
        const moduleSize = module_size | 0;

        const regsPtr = regs_ptr >>> 0;
        const fPtr = f_ptr >>> 0;
        const ePtr = e_ptr >>> 0;
        const aPtr = a_ptr >>> 0;
        const scratchpadPtr = scratchpad_ptr >>> 0;

        console.log(
            '[WASM-JIT-DEBUG] execute module=0x' +
            moduleOffset.toString(16) +
            ' size=' + moduleSize +
            ' regs=0x' + regsPtr.toString(16) +
            ' f=0x' + fPtr.toString(16) +
            ' e=0x' + ePtr.toString(16) +
            ' a=0x' + aPtr.toString(16) +
            ' scratchpad=0x' + scratchpadPtr.toString(16)
        );

        /*
         * --------------------------------------------------------------------
         * Validação básica.
         * --------------------------------------------------------------------
         */

        if (!moduleSize || moduleSize < 0) {
            setError(
                ERR.INVALID_ARGUMENTS,
                'module_size inválido: ' + moduleSize
            );
            return 0;
        }

        /*
         * O limite do HEAP precisa ser verificado antes de HEAPU8.slice().
         */
        const heap = HEAPU8;
        const heapLength = heap.length >>> 0;

        if (moduleOffset > heapLength ||
            moduleSize > heapLength - moduleOffset) {

            setError(
                ERR.INVALID_ARGUMENTS,
                'module_ptr/module_size fora de HEAPU8: ' +
                'ptr=0x' + moduleOffset.toString(16) +
                ' size=' + moduleSize +
                ' heap=' + heapLength
            );

            return 0;
        }

        /*
         * --------------------------------------------------------------------
         * Memória WASM.
         * --------------------------------------------------------------------
         */

        const memory =
            Module['wasmMemory'] ||
            (typeof wasmMemory !== 'undefined' ? wasmMemory : null);

        if (!memory) {
            setError(
                ERR.MEMORY,
                'wasmMemory indisponível'
            );
            return 0;
        }

        if (!memory.buffer) {
            setError(
                ERR.MEMORY,
                'wasmMemory.buffer indisponível'
            );
            return 0;
        }

        const memoryBytes = memory.buffer.byteLength >>> 0;

        console.log(
            '[WASM-JIT] wasmMemory=' +
            memoryBytes +
            ' bytes (' +
            (memoryBytes / 1048576).toFixed(2) +
            ' MiB)'
        );

        /*
         * --------------------------------------------------------------------
         * Os ponteiros precisam estar dentro da memória linear.
         *
         * Não sabemos aqui os tamanhos exatos dos objetos RandomX, então
         * fazemos apenas a validação do início do objeto.
         * --------------------------------------------------------------------
         */

        const checkPointer = (name, ptr) => {
            if (ptr >= memoryBytes) {
                setError(
                    ERR.POINTER_OOB,
                    name +
                    ' fora da memória WASM: 0x' +
                    ptr.toString(16) +
                    ' >= ' +
                    memoryBytes
                );
                return false;
            }

            return true;
        };

        if (!checkPointer('regs', regsPtr)) return 0;
        if (!checkPointer('f', fPtr)) return 0;
        if (!checkPointer('e', ePtr)) return 0;
        if (!checkPointer('a', aPtr)) return 0;
        if (!checkPointer('scratchpad', scratchpadPtr)) return 0;

        /*
         * --------------------------------------------------------------------
         * Copia do módulo.
         *
         * Uint8Array.slice() cria uma cópia independente do HEAP.
         * Isso evita que crescimento/mutação do HEAP altere o módulo enquanto
         * WebAssembly.Module está sendo criado.
         * --------------------------------------------------------------------
         */

        const bytes = heap.slice(
            moduleOffset,
            moduleOffset + moduleSize
        );

        /*
         * --------------------------------------------------------------------
         * Cache.
         *
         * O cache anterior usava apenas:
         *
         *   module_ptr + module_size
         *
         * Isso pode reutilizar incorretamente uma função se o mesmo endereço
         * do HEAP for reutilizado para bytes diferentes.
         *
         * Usamos um hash FNV-1a 32-bit dos bytes.
         * --------------------------------------------------------------------
         */

        let hash = 2166136261 >>> 0;

        /*
         * Hash completo para módulos normais (~10 KB).
         * É barato comparado à compilação de WebAssembly.Module.
         */
        for (let i = 0; i < bytes.length; ++i) {
            hash ^= bytes[i];
            hash = Math.imul(hash, 16777619) >>> 0;
        }

        const key =
            moduleSize.toString() +
            ':' +
            hash.toString(16);

        const cache =
            Module.__randomxJitCache ||
            (Module.__randomxJitCache =
                Object.create(null));

        const stats =
            Module.__randomxJitStats ||
            (Module.__randomxJitStats = {
                executions: 0,
                compilations: 0,
                traps: 0,
                failures: 0
            });

        let fn = cache[key];

        /*
         * --------------------------------------------------------------------
         * Compilação/instanciação.
         * --------------------------------------------------------------------
         */

        if (!fn) {
            console.log(
                '[WASM-JIT] Compilando WebAssembly.Module' +
                ' bytes=' + moduleSize +
                ' hash=0x' + hash.toString(16)
            );

            let wasmModule;

            /*
             * WebAssembly.Module
             */
            try {
                wasmModule = new WebAssembly.Module(bytes);

                console.log(
                    '[WASM-JIT] WebAssembly.Module = OK'
                );
            } catch (e) {
                stats.failures++;

                setError(
                    ERR.MODULE_CREATE,
                    'WebAssembly.Module falhou',
                    e
                );

                console.error(
                    '[WASM-JIT] MODULE_CREATE_NAME:',
                    e && e.name
                );

                console.error(
                    '[WASM-JIT] MODULE_CREATE_MESSAGE:',
                    e && e.message
                );

                console.error(
                    '[WASM-JIT] MODULE_SIZE:',
                    moduleSize
                );

                console.error(
                    '[WASM-JIT] MODULE_HASH:',
                    '0x' + hash.toString(16)
                );

                return 0;
            }

            /*
             * ----------------------------------------------------------------
             * MULH helper
             * ----------------------------------------------------------------
             */

            let mulh = Module.__randomxMulhHelper;

            if (!mulh) {
                try {
                    const helperBytes = Uint8Array.from(
                        atob(
                            'AGFzbQEAAAABBwFgAn5+AX4DAwIAAAcXAghtdWxoX3U2NAAACG11bGhfczY0AAEKngECbwEIfiAAQv////8PgyECIABCIIghAyABQv////8PgyEEIAFCIIghBSACIAR+IQYgAyAEfiEHIAIgBX4hCCAGQiCIIAdC/////w+DfCAIQv////8Pg3whCSAHQiCIIAhCIIh8IAlCIIh8IAMgBX58CywBAn4gACABEAAhAiAAQj+HIAGDIQMgAiADfSECIAFCP4cgAIMhAyACIAN9Cw=='
                        ),
                        c => c.charCodeAt(0)
                    );

                    const helperModule =
                        new WebAssembly.Module(helperBytes);

                    const helperInstance =
                        new WebAssembly.Instance(
                            helperModule,
                            {}
                        );

                    mulh = helperInstance.exports;

                    if (!mulh ||
                        typeof mulh.mulh_u64 !== 'function' ||
                        typeof mulh.mulh_s64 !== 'function') {

                        throw new Error(
                            'MULH exports ausentes'
                        );
                    }

                    Module.__randomxMulhHelper = mulh;

                    console.log(
                        '[WASM-JIT] native WASM mulh helper = ACTIVE'
                    );

                } catch (e) {
                    stats.failures++;

                    setError(
                        ERR.MULH_HELPER,
                        'Falha criando helper MULH',
                        e
                    );

                    return 0;
                }
            }

            /*
             * ----------------------------------------------------------------
             * FP helper
             * ----------------------------------------------------------------
             */

            let fp = Module.__randomxFpHelper;

            if (!fp) {
                try {
                    const fpHelperBytes =
                        Uint8Array.from(
                            atob(
                                'AGFzbQEAAAABEwNgA35+fwF+YAJ+fwF+YAF/AX4DBwYAAAAAAQIHPQYGZnBfYWRkAAAGZnBfc3ViAAEGZnBfbXVsAAIGZnBfZGl2AAMHZnBfc3FydAAEC2ZwX2Zyb21faTMyAAUKPAYKACAAvyABv6C9CwoAIAC/IAG/ob0LCgAgAL8gAb+ivQsKACAAvyABv6O9CwcAIAC/n70LBgAgALe9Cw=='
                            ),
                            c => c.charCodeAt(0)
                        );

                    const fpHelperModule =
                        new WebAssembly.Module(
                            fpHelperBytes
                        );

                    const fpHelperInstance =
                        new WebAssembly.Instance(
                            fpHelperModule,
                            {}
                        );

                    fp = fpHelperInstance.exports;

                    if (!fp ||
                        typeof fp.fp_add !== 'function' ||
                        typeof fp.fp_sub !== 'function' ||
                        typeof fp.fp_mul !== 'function' ||
                        typeof fp.fp_div !== 'function' ||
                        typeof fp.fp_sqrt !== 'function' ||
                        typeof fp.fp_from_i32 !== 'function') {

                        throw new Error(
                            'FP helper exports ausentes'
                        );
                    }

                    Module.__randomxFpHelper = fp;

                    console.log(
                        '[WASM-JIT] native WASM FP helper = ACTIVE'
                    );

                } catch (e) {
                    stats.failures++;

                    setError(
                        ERR.FP_HELPER,
                        'Falha criando helper FP',
                        e
                    );

                    return 0;
                }
            }

            /*
             * ----------------------------------------------------------------
             * IEEE-754 directed rounding helpers.
             * ----------------------------------------------------------------
             */

            const fpBits =
                new DataView(new ArrayBuffer(8));

            const bitsToF64 = (bits) => {
                fpBits.setBigUint64(
                    0,
                    BigInt.asUintN(64, bits),
                    true
                );

                return fpBits.getFloat64(
                    0,
                    true
                );
            };

            const f64ToBits = (value) => {
                fpBits.setFloat64(
                    0,
                    value,
                    true
                );

                return fpBits.getBigUint64(
                    0,
                    true
                );
            };

            const ordered = (bits) => {
                bits = BigInt.asUintN(64, bits);

                return (
                    bits >> 63n
                )
                    ? (
                        ~bits &
                        ((1n << 64n) - 1n)
                    )
                    : (
                        bits |
                        (1n << 63n)
                    );
            };

            const fromOrdered = (x) => {
                const sign =
                    (x >> 63n) & 1n;

                return sign
                    ? (
                        x &
                        ((1n << 63n) - 1n)
                    )
                    : (
                        ~x &
                        ((1n << 64n) - 1n)
                    );
            };

            const nextBits = (
                bits,
                direction
            ) => {
                const v = bitsToF64(bits);

                if (Number.isNaN(v)) {
                    return bits;
                }

                if (v === 0 &&
                    direction < 0) {

                    return 0x8000000000000000n;
                }

                if (v === 0 &&
                    direction > 0) {

                    return 0x0000000000000001n;
                }

                let o = ordered(bits);

                o += direction > 0
                    ? 1n
                    : -1n;

                return fromOrdered(o);
            };

            const exact = (bits) => {
                bits = BigInt.asUintN(
                    64,
                    bits
                );

                const sign =
                    (bits >> 63n)
                        ? -1n
                        : 1n;

                const rawExp =
                    Number(
                        (bits >> 52n) &
                        0x7ffn
                    );

                const frac =
                    bits &
                    0xfffffffffffffn;

                if (rawExp === 0x7ff) {
                    return null;
                }

                const mant =
                    rawExp === 0
                        ? frac
                        : (
                            frac |
                            (1n << 52n)
                        );

                const exp =
                    rawExp === 0
                        ? -1074
                        : rawExp - 1075;

                return {
                    n: sign * mant,
                    e: exp
                };
            };

            const cmpRational = (a, b) => {
                if (a.n === 0n &&
                    b.n === 0n) {

                    return 0;
                }

                if (a.n < 0n &&
                    b.n >= 0) {

                    return -1;
                }

                if (a.n >= 0n &&
                    b.n < 0) {

                    return 1;
                }

                const an =
                    a.n < 0n
                        ? -a.n
                        : a.n;

                const bn =
                    b.n < 0n
                        ? -b.n
                        : b.n;

                const e =
                    Math.min(a.e, b.e);

                const x =
                    an <<
                    BigInt(a.e - e);

                const y =
                    bn <<
                    BigInt(b.e - e);

                if (x === y) {
                    return 0;
                }

                const r =
                    x < y
                        ? -1
                        : 1;

                return a.n < 0n
                    ? -r
                    : r;
            };

            const roundDirected = (
                aBits,
                bBits,
                nearestBits,
                mode,
                op
            ) => {
                if (mode === 0) {
                    return nearestBits;
                }

                const da = exact(aBits);
                const db = exact(bBits);

                if (!da || !db) {
                    return nearestBits;
                }

                let target;

                if (op === 0 ||
                    op === 1) {

                    const e =
                        Math.min(
                            da.e,
                            db.e
                        );

                    const an =
                        da.n <<
                        BigInt(da.e - e);

                    const bn =
                        db.n <<
                        BigInt(db.e - e);

                    target = {
                        n:
                            op === 0
                                ? an + bn
                                : an - bn,
                        e: e
                    };

                } else if (op === 2) {

                    target = {
                        n:
                            da.n * db.n,
                        e:
                            da.e + db.e
                    };

                } else {
                    /*
                     * Para divisão, mantém o resultado IEEE nearest
                     * produzido pelo helper.
                     */
                    return nearestBits;
                }

                const cand =
                    exact(nearestBits);

                if (!cand) {
                    return nearestBits;
                }

                const cmp =
                    cmpRational(
                        cand,
                        target
                    );

                let adjust = 0;

                /*
                 * mode:
                 *
                 * 0 = nearest
                 * 1 = down
                 * 2 = up
                 * 3 = toward zero
                 */

                if (mode === 1 &&
                    cmp > 0) {

                    adjust = -1;
                }

                if (mode === 2 &&
                    cmp < 0) {

                    adjust = 1;
                }

                if (mode === 3) {
                    if (target.n >= 0n &&
                        cmp > 0) {

                        adjust = -1;
                    }

                    if (target.n < 0n &&
                        cmp < 0) {

                        adjust = 1;
                    }
                }

                return adjust
                    ? nextBits(
                        nearestBits,
                        adjust
                    )
                    : nearestBits;
            };

            const fpBin = (
                aBits,
                bBits,
                mode,
                op
            ) => {
                let nearestBits;

                if (op === 0) {
                    nearestBits =
                        fp.fp_add(
                            aBits,
                            bBits,
                            0
                        );

                } else if (op === 1) {
                    nearestBits =
                        fp.fp_sub(
                            aBits,
                            bBits,
                            0
                        );

                } else if (op === 2) {
                    nearestBits =
                        fp.fp_mul(
                            aBits,
                            bBits,
                            0
                        );

                } else {
                    nearestBits =
                        fp.fp_div(
                            aBits,
                            bBits,
                            0
                        );
                }

                return roundDirected(
                    aBits,
                    bBits,
                    nearestBits,
                    mode | 0,
                    op
                );
            };

            const fpSqrt = (
                aBits,
                mode
            ) => {
                return fp.fp_sqrt(
                    aBits,
                    mode | 0
                );
            };

            const fpFromI32 = (
                value
            ) => {
                return fp.fp_from_i32(
                    value | 0
                );
            };

            /*
             * ----------------------------------------------------------------
             * Instanciação do módulo JIT.
             * ----------------------------------------------------------------
             */

            let instance;

            try {
                console.log(
                    '[WASM-JIT] Instanciando rx_jit...'
                );

                instance =
                    new WebAssembly.Instance(
                        wasmModule,
                        {
                            env: {
                                memory: memory,

                                mulh_u64:
                                    mulh.mulh_u64,

                                mulh_s64:
                                    mulh.mulh_s64,

                                fp_add:
                                    (
                                        aa,
                                        bb,
                                        mode
                                    ) => {
                                        return fpBin(
                                            aa,
                                            bb,
                                            mode,
                                            0
                                        );
                                    },

                                fp_sub:
                                    (
                                        aa,
                                        bb,
                                        mode
                                    ) => {
                                        return fpBin(
                                            aa,
                                            bb,
                                            mode,
                                            1
                                        );
                                    },

                                fp_mul:
                                    (
                                        aa,
                                        bb,
                                        mode
                                    ) => {
                                        return fpBin(
                                            aa,
                                            bb,
                                            mode,
                                            2
                                        );
                                    },

                                fp_div:
                                    (
                                        aa,
                                        bb,
                                        mode
                                    ) => {
                                        return fpBin(
                                            aa,
                                            bb,
                                            mode,
                                            3
                                        );
                                    },

                                fp_sqrt:
                                    (
                                        aa,
                                        mode
                                    ) => {
                                        return fpSqrt(
                                            aa,
                                            mode
                                        );
                                    },

                                fp_from_i32:
                                    fpFromI32
                            }
                        }
                    );

                console.log(
                    '[WASM-JIT] WebAssembly.Instance = OK'
                );

            } catch (e) {
                stats.failures++;

                setError(
                    ERR.INSTANCE_CREATE,
                    'WebAssembly.Instance falhou',
                    e
                );

                console.error(
                    '[WASM-JIT] INSTANCE_CREATE_NAME:',
                    e && e.name
                );

                console.error(
                    '[WASM-JIT] INSTANCE_CREATE_MESSAGE:',
                    e && e.message
                );

                console.error(
                    '[WASM-JIT] MODULE_HASH:',
                    '0x' + hash.toString(16)
                );

                return 0;
            }

            /*
             * ----------------------------------------------------------------
             * Export rx_jit.
             * ----------------------------------------------------------------
             */

            fn =
                instance &&
                instance.exports
                    ? instance.exports.rx_jit
                    : null;

            if (typeof fn !== 'function') {
                stats.failures++;

                setError(
                    ERR.EXPORT_MISSING,
                    'export rx_jit ausente'
                );

                if (instance &&
                    instance.exports) {

                    try {
                        console.error(
                            '[WASM-JIT] exports:',
                            Object.keys(
                                instance.exports
                            )
                        );
                    } catch (_) {
                        /* ignore */
                    }
                }

                return 0;
            }

            cache[key] = fn;
            stats.compilations++;

            console.log(
                '[WASM-JIT] WebAssembly.Instance = OK; ' +
                'rx_jit = ACTIVE'
            );

            console.log(
                '[WASM-JIT] cache key=' + key
            );
        }

        /*
         * --------------------------------------------------------------------
         * Executar rx_jit.
         * --------------------------------------------------------------------
         */

        console.log(
            '[WASM-JIT-DEBUG] BEFORE rx_jit'
        );

        try {
            /*
             * Explicitamente i32.
             *
             * Os parâmetros do módulo JIT são offsets WASM32.
             */
            const result = fn(
                regsPtr | 0,
                fPtr | 0,
                ePtr | 0,
                aPtr | 0,
                scratchpadPtr | 0
            );

            console.log(
                '[WASM-JIT-DEBUG] AFTER rx_jit',
                'return=' + String(result)
            );

        } catch (e) {
            stats.traps++;
            stats.failures++;

            setError(
                ERR.EXECUTE_TRAP,
                'rx_jit causou trap',
                e
            );

            console.error(
                '[WASM-JIT] RX_JIT_TRAP_NAME:',
                e && e.name
            );

            console.error(
                '[WASM-JIT] RX_JIT_TRAP_MESSAGE:',
                e && e.message
            );

            console.error(
                '[WASM-JIT] RX_JIT_TRAP_STACK:',
                e && e.stack
            );

            return 0;
        }

        /*
         * --------------------------------------------------------------------
         * Sucesso.
         * --------------------------------------------------------------------
         */

        stats.executions++;

        if (stats.executions === 1 ||
            (stats.executions % 10000) === 0) {

            console.log(
                '[WASM-JIT] execucoes=' +
                stats.executions +
                ' compilacoes=' +
                stats.compilations +
                ' traps=' +
                stats.traps +
                ' failures=' +
                stats.failures
            );
        }

        return 1;

    } catch (e) {
        /*
         * --------------------------------------------------------------------
         * Catch global.
         * --------------------------------------------------------------------
         */

        if (Module.__randomxJitStats) {
            Module.__randomxJitStats.failures++;
        }

        setError(
            ERR.RUNTIME,
            'runtime error inesperado',
            e
        );

        console.error(
            '[WASM-JIT] RUNTIME_ERROR_NAME:',
            e && e.name
        );

        console.error(
            '[WASM-JIT] RUNTIME_ERROR_MESSAGE:',
            e && e.message
        );

        console.error(
            '[WASM-JIT] RUNTIME_ERROR_STACK:',
            e && e.stack
        );

        return 0;
    }
});


/*
 * ============================================================================
 * WasmJit::execute
 * ============================================================================
 */

} // namespace


bool WasmJit::execute(
    uint8_t* regs,
    uint8_t* f,
    uint8_t* e,
    uint8_t* a,
    uint8_t* scratchpad)
{
    fprintf(
        stderr,
        "[WASM-JIT-DEBUG] execute: "
        "module=%p size=%zu "
        "regs=%p f=%p e=%p a=%p scratchpad=%p\n",
        module_.data(),
        module_.size(),
        regs,
        f,
        e,
        a,
        scratchpad
    );

    /*
     * ------------------------------------------------------------------------
     * Validação do módulo.
     * ------------------------------------------------------------------------
     */

    if (module_.empty()) {
        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: module empty\n"
        );
        return false;
    }

    /*
     * randomx_wasm_execute_module recebe int para o tamanho.
     */
    if (module_.size() >
        static_cast<size_t>(INT_MAX)) {

        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: module too large: %zu\n",
            module_.size()
        );

        return false;
    }

    /*
     * ------------------------------------------------------------------------
     * Validação dos ponteiros.
     * ------------------------------------------------------------------------
     */

    if (!regs) {
        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: regs == nullptr\n"
        );
        return false;
    }

    if (!f) {
        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: f == nullptr\n"
        );
        return false;
    }

    if (!e) {
        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: e == nullptr\n"
        );
        return false;
    }

    if (!a) {
        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: a == nullptr\n"
        );
        return false;
    }

    if (!scratchpad) {
        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: scratchpad == nullptr\n"
        );
        return false;
    }

    /*
     * ------------------------------------------------------------------------
     * Ponteiros WASM32.
     *
     * Emscripten WebAssembly atual é WASM32, portanto os offsets são i32.
     * ------------------------------------------------------------------------
     */

    const uintptr_t regsAddress =
        reinterpret_cast<uintptr_t>(regs);

    const uintptr_t fAddress =
        reinterpret_cast<uintptr_t>(f);

    const uintptr_t eAddress =
        reinterpret_cast<uintptr_t>(e);

    const uintptr_t aAddress =
        reinterpret_cast<uintptr_t>(a);

    const uintptr_t scratchpadAddress =
        reinterpret_cast<uintptr_t>(scratchpad);

    /*
     * Se algum endereço não couber em uint32_t, o JIT WASM32 não pode
     * representá-lo como i32.
     */
    if (regsAddress >
            static_cast<uintptr_t>(UINT32_MAX) ||
        fAddress >
            static_cast<uintptr_t>(UINT32_MAX) ||
        eAddress >
            static_cast<uintptr_t>(UINT32_MAX) ||
        aAddress >
            static_cast<uintptr_t>(UINT32_MAX) ||
        scratchpadAddress >
            static_cast<uintptr_t>(UINT32_MAX)) {

        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] FAIL: pointer > WASM32 address space\n"
        );

        fprintf(
            stderr,
            "[WASM-JIT-DEBUG] regs=%p f=%p e=%p a=%p scratchpad=%p\n",
            regs,
            f,
            e,
            a,
            scratchpad
        );

        return false;
    }

    const int regsPtr =
        static_cast<int>(
            static_cast<uint32_t>(regsAddress)
        );

    const int fPtr =
        static_cast<int>(
            static_cast<uint32_t>(fAddress)
        );

    const int ePtr =
        static_cast<int>(
            static_cast<uint32_t>(eAddress)
        );

    const int aPtr =
        static_cast<int>(
            static_cast<uint32_t>(aAddress)
        );

    const int scratchpadPtr =
        static_cast<int>(
            static_cast<uint32_t>(scratchpadAddress)
        );

    fprintf(
        stderr,
        "[WASM-JIT-DEBUG] WASM32 args: "
        "regs=0x%08x "
        "f=0x%08x "
        "e=0x%08x "
        "a=0x%08x "
        "scratchpad=0x%08x\n",
        static_cast<unsigned>(regsPtr),
        static_cast<unsigned>(fPtr),
        static_cast<unsigned>(ePtr),
        static_cast<unsigned>(aPtr),
        static_cast<unsigned>(scratchpadPtr)
    );

    /*
     * ------------------------------------------------------------------------
     * Executar.
     * ------------------------------------------------------------------------
     */

    const int status =
        randomx_wasm_execute_module(
            module_.data(),
            static_cast<int>(module_.size()),
            regsPtr,
            fPtr,
            ePtr,
            aPtr,
            scratchpadPtr
        );

    fprintf(
        stderr,
        "[WASM-JIT-DEBUG] "
        "randomx_wasm_execute_module -> %d\n",
        status
    );

    if (status == 1) {
        return true;
    }

    /*
     * ------------------------------------------------------------------------
     * Falha.
     * ------------------------------------------------------------------------
     */

    fprintf(
        stderr,
        "[WASM-JIT-DEBUG] "
        "JIT execution FAILED\n"
    );

    return false;
}


} // namespace randomx

#endif // __EMSCRIPTEN__
