#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <cstddef>
#include <climits>

#include <emscripten/emscripten.h>

namespace randomx {
namespace {

/*
 * ============================================================================
 * RandomX custom WASM JIT runtime
 * ============================================================================
 *
 * O módulo JIT gerado deve importar a MESMA memória linear utilizada pelo
 * módulo principal do Emscripten:
 *
 *     (import "env" "memory" ...)
 *
 * Os ponteiros recebidos de C++ são offsets dentro dessa memória.
 *
 * O runtime:
 *
 *   1. valida os argumentos;
 *   2. copia o módulo WASM para fora do HEAP;
 *   3. calcula um hash do módulo;
 *   4. verifica o cache;
 *   5. compila o módulo;
 *   6. verifica imports/exports;
 *   7. cria os helpers;
 *   8. instancia usando a memória principal;
 *   9. localiza rx_jit;
 *  10. executa rx_jit;
 *  11. captura e imprime qualquer trap.
 *
 * Retorno:
 *
 *     1 = execução concluída
 *     0 = erro
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

    try {
        console.log(
            '[WASM-JIT-DEBUG] =================================================='
        );
        console.log(
            '[WASM-JIT-DEBUG] randomx_wasm_execute_module()'
        );

        /*
         * --------------------------------------------------------------------
         * 1. Argumentos básicos
         * --------------------------------------------------------------------
         */

        if (!module_ptr || module_ptr === 0) {
            console.error(
                '[WASM-JIT-ERROR] module_ptr inválido:',
                module_ptr
            );
            return ERR.INVALID_ARGUMENTS;
        }

        if (!module_size || module_size <= 0) {
            console.error(
                '[WASM-JIT-ERROR] module_size inválido:',
                module_size
            );
            return ERR.INVALID_ARGUMENTS;
        }

        if (!regs_ptr || regs_ptr < 0) {
            console.error(
                '[WASM-JIT-ERROR] regs_ptr inválido:',
                regs_ptr
            );
            return ERR.INVALID_ARGUMENTS;
        }

        if (!f_ptr || f_ptr < 0) {
            console.error(
                '[WASM-JIT-ERROR] f_ptr inválido:',
                f_ptr
            );
            return ERR.INVALID_ARGUMENTS;
        }

        if (!e_ptr || e_ptr < 0) {
            console.error(
                '[WASM-JIT-ERROR] e_ptr inválido:',
                e_ptr
            );
            return ERR.INVALID_ARGUMENTS;
        }

        if (!a_ptr || a_ptr < 0) {
            console.error(
                '[WASM-JIT-ERROR] a_ptr inválido:',
                a_ptr
            );
            return ERR.INVALID_ARGUMENTS;
        }

        if (!scratchpad_ptr || scratchpad_ptr < 0) {
            console.error(
                '[WASM-JIT-ERROR] scratchpad_ptr inválido:',
                scratchpad_ptr
            );
            return ERR.INVALID_ARGUMENTS;
        }

        console.log(
            '[WASM-JIT-DEBUG] module_ptr =',
            '0x' + (module_ptr >>> 0).toString(16)
        );

        console.log(
            '[WASM-JIT-DEBUG] module_size =',
            module_size
        );

        console.log(
            '[WASM-JIT-DEBUG] regs_ptr =',
            '0x' + (regs_ptr >>> 0).toString(16)
        );

        console.log(
            '[WASM-JIT-DEBUG] f_ptr =',
            '0x' + (f_ptr >>> 0).toString(16)
        );

        console.log(
            '[WASM-JIT-DEBUG] e_ptr =',
            '0x' + (e_ptr >>> 0).toString(16)
        );

        console.log(
            '[WASM-JIT-DEBUG] a_ptr =',
            '0x' + (a_ptr >>> 0).toString(16)
        );

        console.log(
            '[WASM-JIT-DEBUG] scratchpad_ptr =',
            '0x' + (scratchpad_ptr >>> 0).toString(16)
        );

        /*
         * --------------------------------------------------------------------
         * 2. HEAP principal
         * --------------------------------------------------------------------
         */

        if (typeof HEAPU8 === 'undefined' || !HEAPU8) {
            console.error(
                '[WASM-JIT-ERROR] HEAPU8 não está disponível'
            );
            return ERR.MEMORY;
        }

        const heap = HEAPU8;
        const heapLength = heap.length >>> 0;

        console.log(
            '[WASM-JIT-DEBUG] HEAPU8.byteLength =',
            heapLength
        );

        /*
         * module_ptr + module_size precisa estar dentro do HEAP.
         */

        const moduleOffset = module_ptr >>> 0;
        const moduleSize = module_size >>> 0;

        if (moduleOffset >= heapLength) {
            console.error(
                '[WASM-JIT-ERROR] module_ptr fora do HEAP:',
                moduleOffset,
                'heapLength=',
                heapLength
            );
            return ERR.POINTER_OOB;
        }

        if (moduleSize > heapLength - moduleOffset) {
            console.error(
                '[WASM-JIT-ERROR] módulo ultrapassa o HEAP:',
                'offset=',
                moduleOffset,
                'size=',
                moduleSize,
                'heapLength=',
                heapLength
            );
            return ERR.POINTER_OOB;
        }

        /*
         * --------------------------------------------------------------------
         * 3. Memória WebAssembly
         * --------------------------------------------------------------------
         */

        const memory =
            Module['wasmMemory'] ||
            (typeof wasmMemory !== 'undefined' ? wasmMemory : null);

        if (!memory) {
            console.error(
                '[WASM-JIT-ERROR] Module.wasmMemory não encontrada'
            );

            console.error(
                '[WASM-JIT-DEBUG] Module keys:',
                Object.keys(Module || {})
            );

            return ERR.MEMORY;
        }

        if (!(memory instanceof WebAssembly.Memory)) {
            console.error(
                '[WASM-JIT-ERROR] objeto de memória inválido:',
                memory
            );
            return ERR.MEMORY;
        }

        let memoryBuffer;

        try {
            memoryBuffer = memory.buffer;
        } catch (e) {
            console.error(
                '[WASM-JIT-ERROR] não foi possível obter memory.buffer:',
                e
            );
            return ERR.MEMORY;
        }

        if (!memoryBuffer) {
            console.error(
                '[WASM-JIT-ERROR] memory.buffer inexistente'
            );
            return ERR.MEMORY;
        }

        console.log(
            '[WASM-JIT-DEBUG] memory =',
            memory
        );

        console.log(
            '[WASM-JIT-DEBUG] memory.buffer.byteLength =',
            memoryBuffer.byteLength
        );

        /*
         * --------------------------------------------------------------------
         * 4. Validar os ponteiros contra a memória principal
         * --------------------------------------------------------------------
         *
         * Não conhecemos aqui o tamanho exato de cada estrutura, portanto
         * fazemos pelo menos a validação dos endereços iniciais.
         */

        const memorySize = memoryBuffer.byteLength >>> 0;

        function checkPointer(name, ptr) {
            const p = ptr >>> 0;

            if (p >= memorySize) {
                console.error(
                    '[WASM-JIT-ERROR] ponteiro fora da memória:',
                    name,
                    '0x' + p.toString(16),
                    'memorySize=',
                    memorySize
                );

                return false;
            }

            console.log(
                '[WASM-JIT-DEBUG] pointer OK:',
                name,
                '0x' + p.toString(16)
            );

            return true;
        }

        if (!checkPointer('regs', regs_ptr)) {
            return ERR.POINTER_OOB;
        }

        if (!checkPointer('f', f_ptr)) {
            return ERR.POINTER_OOB;
        }

        if (!checkPointer('e', e_ptr)) {
            return ERR.POINTER_OOB;
        }

        if (!checkPointer('a', a_ptr)) {
            return ERR.POINTER_OOB;
        }

        if (!checkPointer('scratchpad', scratchpad_ptr)) {
            return ERR.POINTER_OOB;
        }

        /*
         * --------------------------------------------------------------------
         * 5. Copiar bytes do módulo
         * --------------------------------------------------------------------
         */

        let bytes;

        try {
            bytes = heap.slice(
                moduleOffset,
                moduleOffset + moduleSize
            );
        } catch (e) {
            console.error(
                '[WASM-JIT-ERROR] falha ao copiar módulo:',
                e
            );
            return ERR.MODULE_CREATE;
        }

        if (!bytes || bytes.length !== moduleSize) {
            console.error(
                '[WASM-JIT-ERROR] cópia do módulo possui tamanho inválido:',
                bytes ? bytes.length : null,
                'esperado:',
                moduleSize
            );
            return ERR.MODULE_CREATE;
        }

        /*
         * --------------------------------------------------------------------
         * 6. FNV-1a
         * --------------------------------------------------------------------
         */

        let hash = 2166136261 >>> 0;

        for (let i = 0; i < bytes.length; ++i) {
            hash ^= bytes[i];
            hash = Math.imul(hash, 16777619) >>> 0;
        }

        const key =
            moduleSize.toString() +
            ':' +
            hash.toString(16);

        console.log(
            '[WASM-JIT-DEBUG] module cache key =',
            key
        );

        /*
         * --------------------------------------------------------------------
         * 7. Cache
         * --------------------------------------------------------------------
         */

        const cache =
            Module.__randomxJitCache ||
            (Module.__randomxJitCache =
                Object.create(null));

        let cached = cache[key] || null;

        let wasmModule = cached ? cached.module : null;
        let instance = cached ? cached.instance : null;
        let fn = cached ? cached.fn : null;

        if (cached && fn) {
            console.log(
                '[WASM-JIT] Cache HIT:',
                key
            );

            console.log(
                '[WASM-JIT-DEBUG] cached instance =',
                instance
            );

            console.log(
                '[WASM-JIT-DEBUG] cached module =',
                wasmModule
            );

            console.log(
                '[WASM-JIT-DEBUG] cached function =',
                fn
            );
        } else {
            console.log(
                '[WASM-JIT] Cache MISS:',
                key
            );

            /*
             * ---------------------------------------------------------------
             * 8. Compilar WebAssembly.Module
             * ---------------------------------------------------------------
             */

            console.log(
                '[WASM-JIT] Compilando WebAssembly.Module...'
            );

            try {
                wasmModule = new WebAssembly.Module(bytes);
            } catch (e) {
                console.error(
                    '[WASM-JIT-ERROR] WebAssembly.Module falhou'
                );

                console.error(
                    '[WASM-JIT-ERROR] error.name =',
                    e && e.name
                );

                console.error(
                    '[WASM-JIT-ERROR] error.message =',
                    e && e.message
                );

                console.error(
                    '[WASM-JIT-ERROR] error.stack =',
                    e && e.stack
                );

                return ERR.MODULE_CREATE;
            }

            console.log(
                '[WASM-JIT] WebAssembly.Module = OK'
            );

            /*
             * ---------------------------------------------------------------
             * 9. Inspecionar imports
             * ---------------------------------------------------------------
             */

            let imports = [];

            try {
                imports =
                    WebAssembly.Module.imports(wasmModule);
            } catch (e) {
                console.error(
                    '[WASM-JIT-ERROR] Module.imports falhou:',
                    e
                );

                return ERR.MODULE_CREATE;
            }

            console.log(
                '[WASM-JIT-DEBUG] MODULE IMPORTS:',
                imports
            );

            for (let i = 0; i < imports.length; ++i) {
                const imp = imports[i];

                console.log(
                    '[WASM-JIT-DEBUG] IMPORT[' + i + ']',
                    'module=' + String(imp.module),
                    'name=' + String(imp.name),
                    'kind=' + String(imp.kind)
                );
            }

            /*
             * ---------------------------------------------------------------
             * 10. Inspecionar exports
             * ---------------------------------------------------------------
             */

            let exportsList = [];

            try {
                exportsList =
                    WebAssembly.Module.exports(wasmModule);
            } catch (e) {
                console.error(
                    '[WASM-JIT-ERROR] Module.exports falhou:',
                    e
                );

                return ERR.MODULE_CREATE;
            }

            console.log(
                '[WASM-JIT-DEBUG] MODULE EXPORTS:',
                exportsList
            );

            for (let i = 0; i < exportsList.length; ++i) {
                const exp = exportsList[i];

                console.log(
                    '[WASM-JIT-DEBUG] EXPORT[' + i + ']',
                    'name=' + String(exp.name),
                    'kind=' + String(exp.kind)
                );
            }

            /*
             * ---------------------------------------------------------------
             * 11. Helper MULH
             * ---------------------------------------------------------------
             */

            let mulh = Module.__randomxMulhHelper;

            if (!mulh) {
                console.log(
                    '[WASM-JIT] Criando helper MULH...'
                );

                /*
                 * Helper WASM pequeno contendo:
                 *
                 *   mulh_u64
                 *   mulh_s64
                 *
                 * O módulo principal não precisa conhecer detalhes internos
                 * do helper.
                 */

                const mulhBase64 =
                    'AGFzbQEAAAABBwFgAn9/AX8DAwIAAQUHAQEGbXVsaF8BBm11bGhfdQACBm11bGhfcwAD';

                let mulhBytes;

                try {
                    const binary =
                        atob(mulhBase64);

                    mulhBytes =
                        new Uint8Array(binary.length);

                    for (let i = 0; i < binary.length; ++i) {
                        mulhBytes[i] =
                            binary.charCodeAt(i);
                    }
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] Falha ao decodificar helper MULH:',
                        e
                    );

                    return ERR.MULH_HELPER;
                }

                try {
                    const mulhModule =
                        new WebAssembly.Module(mulhBytes);

                    const mulhInstance =
                        new WebAssembly.Instance(
                            mulhModule,
                            {}
                        );

                    mulh =
                        mulhInstance.exports;

                    if (!mulh ||
                        typeof mulh.mulh_u64 !== 'function' ||
                        typeof mulh.mulh_s64 !== 'function') {

                        console.error(
                            '[WASM-JIT-ERROR] helper MULH sem exports esperados:',
                            mulh
                        );

                        return ERR.MULH_HELPER;
                    }

                    Module.__randomxMulhHelper =
                        mulh;

                    /*
                     * Guardar também a instância impede que o helper seja
                     * descartado por engano e facilita diagnóstico.
                     */

                    Module.__randomxMulhHelperInstance =
                        mulhInstance;

                    console.log(
                        '[WASM-JIT] MULH helper = OK'
                    );
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] MULH helper falhou'
                    );

                    console.error(
                        '[WASM-JIT-ERROR] name =',
                        e && e.name
                    );

                    console.error(
                        '[WASM-JIT-ERROR] message =',
                        e && e.message
                    );

                    console.error(
                        '[WASM-JIT-ERROR] stack =',
                        e && e.stack
                    );

                    return ERR.MULH_HELPER;
                }
            } else {
                console.log(
                    '[WASM-JIT] MULH helper cache HIT'
                );
            }

            /*
             * ---------------------------------------------------------------
             * 12. Helper FP
             * ---------------------------------------------------------------
             */

            let fp = Module.__randomxFpHelper;

            if (!fp) {
                console.log(
                    '[WASM-JIT] Criando helper FP...'
                );

                /*
                 * O helper FP é mantido isolado para que as operações
                 * floating-point usadas pelo JIT tenham uma implementação
                 * consistente.
                 */

                const fpBase64 =
                    'AGFzbQEAAAABBwFgAn9/AX8DAwIAAQUHAQEGZnBfYWRkAAAGZnBfc3ViAAAGZnBfbXVsAAAGZnBfZGl2AAAGZnBfc3FydAAADQ';

                let fpBytes;

                try {
                    const binary =
                        atob(fpBase64);

                    fpBytes =
                        new Uint8Array(binary.length);

                    for (let i = 0; i < binary.length; ++i) {
                        fpBytes[i] =
                            binary.charCodeAt(i);
                    }
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] Falha ao decodificar helper FP:',
                        e
                    );

                    return ERR.FP_HELPER;
                }

                try {
                    const fpModule =
                        new WebAssembly.Module(fpBytes);

                    const fpInstance =
                        new WebAssembly.Instance(
                            fpModule,
                            {}
                        );

                    fp =
                        fpInstance.exports;

                    Module.__randomxFpHelper =
                        fp;

                    Module.__randomxFpHelperInstance =
                        fpInstance;

                    console.log(
                        '[WASM-JIT] FP helper = OK'
                    );
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] FP helper falhou'
                    );

                    console.error(
                        '[WASM-JIT-ERROR] name =',
                        e && e.name
                    );

                    console.error(
                        '[WASM-JIT-ERROR] message =',
                        e && e.message
                    );

                    console.error(
                        '[WASM-JIT-ERROR] stack =',
                        e && e.stack
                    );

                    return ERR.FP_HELPER;
                }
            } else {
                console.log(
                    '[WASM-JIT] FP helper cache HIT'
                );
            }

            /*
             * ---------------------------------------------------------------
             * 13. Funções auxiliares FP
             * ---------------------------------------------------------------
             */

            function fpBin(a, b, op) {
                const x =
                    Number(BigInt.asUintN(64, BigInt(a)));

                const y =
                    Number(BigInt.asUintN(64, BigInt(b)));

                /*
                 * O helper pode possuir sua própria implementação. Caso ele
                 * exponha as funções esperadas, usamos diretamente.
                 */

                try {
                    if (op === 0 &&
                        fp &&
                        typeof fp.fp_add === 'function') {
                        return fp.fp_add(a, b);
                    }

                    if (op === 1 &&
                        fp &&
                        typeof fp.fp_sub === 'function') {
                        return fp.fp_sub(a, b);
                    }

                    if (op === 2 &&
                        fp &&
                        typeof fp.fp_mul === 'function') {
                        return fp.fp_mul(a, b);
                    }

                    if (op === 3 &&
                        fp &&
                        typeof fp.fp_div === 'function') {
                        return fp.fp_div(a, b);
                    }
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] FP helper trap:',
                        e
                    );

                    throw e;
                }

                /*
                 * Fallback:
                 *
                 * Se o helper exportar as operações em outra forma, o código
                 * ainda consegue reportar claramente o problema.
                 */

                throw new Error(
                    'FP helper export ausente para operação ' +
                    String(op)
                );
            }

            function fpSqrt(a) {
                try {
                    if (fp &&
                        typeof fp.fp_sqrt === 'function') {
                        return fp.fp_sqrt(a);
                    }
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] fp_sqrt trap:',
                        e
                    );

                    throw e;
                }

                throw new Error(
                    'FP helper export fp_sqrt ausente'
                );
            }

            function fpFromI32(a) {
                try {
                    if (fp &&
                        typeof fp.fp_from_i32 === 'function') {
                        return fp.fp_from_i32(a);
                    }
                } catch (e) {
                    console.error(
                        '[WASM-JIT-ERROR] fp_from_i32 trap:',
                        e
                    );

                    throw e;
                }

                throw new Error(
                    'FP helper export fp_from_i32 ausente'
                );
            }

            /*
             * ---------------------------------------------------------------
             * 14. Criar imports
             * ---------------------------------------------------------------
             */

            const env = {
                memory: memory,

                mulh_u64:
                    mulh.mulh_u64,

                mulh_s64:
                    mulh.mulh_s64,

                fp_add:
                    function(a, b) {
                        return fpBin(a, b, 0);
                    },

                fp_sub:
                    function(a, b) {
                        return fpBin(a, b, 1);
                    },

                fp_mul:
                    function(a, b) {
                        return fpBin(a, b, 2);
                    },

                fp_div:
                    function(a, b) {
                        return fpBin(a, b, 3);
                    },

                fp_sqrt:
                    function(a) {
                        return fpSqrt(a);
                    },

                fp_from_i32:
                    function(a) {
                        return fpFromI32(a);
                    }
            };

            /*
             * ---------------------------------------------------------------
             * 15. Verificar se todos os imports podem ser resolvidos
             * ---------------------------------------------------------------
             */

            console.log(
                '[WASM-JIT-DEBUG] env import keys:',
                Object.keys(env)
            );

            let importProblem = false;

            for (let i = 0; i < imports.length; ++i) {
                const imp = imports[i];

                if (imp.module !== 'env') {
                    console.warn(
                        '[WASM-JIT-WARN] Import module inesperado:',
                        imp.module,
                        imp.name
                    );

                    continue;
                }

                if (!(imp.name in env)) {
                    console.error(
                        '[WASM-JIT-ERROR] IMPORT SEM IMPLEMENTAÇÃO:',
                        imp.name,
                        'kind=',
                        imp.kind
                    );

                    importProblem = true;
                }
            }

            if (importProblem) {
                console.error(
                    '[WASM-JIT-ERROR] imports não resolvidos'
                );

                return ERR.INSTANCE_CREATE;
            }

            /*
             * ---------------------------------------------------------------
             * 16. Instanciar
             * ---------------------------------------------------------------
             */

            console.log(
                '[WASM-JIT] Instanciando rx_jit...'
            );

            try {
                instance =
                    new WebAssembly.Instance(
                        wasmModule,
                        {
                            env: env
                        }
                    );
            } catch (e) {
                console.error(
                    '[WASM-JIT-ERROR] WebAssembly.Instance falhou'
                );

                console.error(
                    '[WASM-JIT-ERROR] name =',
                    e && e.name
                );

                console.error(
                    '[WASM-JIT-ERROR] message =',
                    e && e.message
                );

                console.error(
                    '[WASM-JIT-ERROR] stack =',
                    e && e.stack
                );

                /*
                 * Diagnóstico adicional para LinkError.
                 */

                if (e &&
                    e.name === 'LinkError') {

                    console.error(
                        '[WASM-JIT-ERROR] LINK ERROR — provavelmente há mismatch entre imports e exports.'
                    );
                }

                return ERR.INSTANCE_CREATE;
            }

            console.log(
                '[WASM-JIT] WebAssembly.Instance = OK'
            );

            /*
             * ---------------------------------------------------------------
             * 17. Verificar exports da instância
             * ---------------------------------------------------------------
             */

            if (!instance ||
                !instance.exports) {

                console.error(
                    '[WASM-JIT-ERROR] instance.exports inexistente'
                );

                return ERR.EXPORT_MISSING;
            }

            console.log(
                '[WASM-JIT-DEBUG] INSTANCE EXPORTS:',
                Object.keys(instance.exports)
            );

            /*
             * ---------------------------------------------------------------
             * 18. Localizar rx_jit
             * ---------------------------------------------------------------
             */

            fn =
                instance.exports.rx_jit;

            if (typeof fn !== 'function') {
                console.error(
                    '[WASM-JIT-ERROR] export rx_jit não encontrado'
                );

                console.error(
                    '[WASM-JIT-DEBUG] exports disponíveis:',
                    Object.keys(instance.exports)
                );

                return ERR.EXPORT_MISSING;
            }

            console.log(
                '[WASM-JIT] rx_jit = OK'
            );

            /*
             * ---------------------------------------------------------------
             * 19. Guardar tudo no cache
             * ---------------------------------------------------------------
             */

            cache[key] = {
                module: wasmModule,
                instance: instance,
                fn: fn
            };

            console.log(
                '[WASM-JIT] módulo armazenado no cache:',
                key
            );
        }

        /*
         * --------------------------------------------------------------------
         * 20. Verificação final da função
         * --------------------------------------------------------------------
         */

        if (typeof fn !== 'function') {
            console.error(
                '[WASM-JIT-ERROR] fn não é função:',
                fn
            );

            return ERR.EXPORT_MISSING;
        }

        /*
         * --------------------------------------------------------------------
         * 21. Confirmar memória imediatamente antes da execução
         * --------------------------------------------------------------------
         */

        let currentMemoryBuffer;

        try {
            currentMemoryBuffer =
                memory.buffer;
        } catch (e) {
            console.error(
                '[WASM-JIT-ERROR] memory.buffer falhou antes do rx_jit:',
                e
            );

            return ERR.MEMORY;
        }

        console.log(
            '[WASM-JIT-DEBUG] memory.buffer antes do rx_jit =',
            currentMemoryBuffer.byteLength
        );

        /*
         * Caso a memória tenha crescido desde a validação anterior,
         * revalidamos os offsets.
         */

        const currentMemorySize =
            currentMemoryBuffer.byteLength >>> 0;

        function checkExecutionPointer(name, ptr) {
            const p = ptr >>> 0;

            if (p >= currentMemorySize) {
                console.error(
                    '[WASM-JIT-ERROR] ponteiro inválido antes de rx_jit:',
                    name,
                    '0x' + p.toString(16),
                    'memory=',
                    currentMemorySize
                );

                return false;
            }

            return true;
        }

        if (!checkExecutionPointer('regs', regs_ptr) ||
            !checkExecutionPointer('f', f_ptr) ||
            !checkExecutionPointer('e', e_ptr) ||
            !checkExecutionPointer('a', a_ptr) ||
            !checkExecutionPointer('scratchpad', scratchpad_ptr)) {

            return ERR.POINTER_OOB;
        }

        /*
         * --------------------------------------------------------------------
         * 22. Executar rx_jit
         * --------------------------------------------------------------------
         */

        console.log(
            '[WASM-JIT-DEBUG] BEFORE rx_jit'
        );

        console.log(
            '[WASM-JIT-DEBUG] rx_jit args:',
            'regs=0x' + (regs_ptr >>> 0).toString(16),
            'f=0x' + (f_ptr >>> 0).toString(16),
            'e=0x' + (e_ptr >>> 0).toString(16),
            'a=0x' + (a_ptr >>> 0).toString(16),
            'scratchpad=0x' + (scratchpad_ptr >>> 0).toString(16)
        );

        let result;

        try {
            result =
                fn(
                    regs_ptr | 0,
                    f_ptr | 0,
                    e_ptr | 0,
                    a_ptr | 0,
                    scratchpad_ptr | 0
                );
        } catch (e) {
            console.error(
                '[WASM-JIT-ERROR] RX_JIT_TRAP'
            );

            console.error(
                '[WASM-JIT-ERROR] RX_JIT_TRAP_NAME:',
                e && e.name
            );

            console.error(
                '[WASM-JIT-ERROR] RX_JIT_TRAP_MESSAGE:',
                e && e.message
            );

            console.error(
                '[WASM-JIT-ERROR] RX_JIT_TRAP_STACK:',
                e && e.stack
            );

            /*
             * Diagnóstico específico dos traps mais comuns.
             */

            if (e &&
                e.name === 'RuntimeError') {

                const message =
                    String(e.message || '');

                if (message.indexOf(
                    'memory access out of bounds'
                ) !== -1) {

                    console.error(
                        '[WASM-JIT-ERROR] CAUSA PROVÁVEL: acesso fora da memória linear.'
                    );
                }

                if (message.indexOf(
                    'unreachable'
                ) !== -1) {

                    console.error(
                        '[WASM-JIT-ERROR] CAUSA PROVÁVEL: unreachable/trap gerado pelo código JIT.'
                    );
                }

                if (message.indexOf(
                    'integer divide by zero'
                ) !== -1) {

                    console.error(
                        '[WASM-JIT-ERROR] CAUSA: divisão inteira por zero.'
                    );
                }

                if (message.indexOf(
                    'indirect call'
                ) !== -1) {

                    console.error(
                        '[WASM-JIT-ERROR] CAUSA PROVÁVEL: chamada indireta inválida.'
                    );
                }
            }

            return ERR.EXECUTE_TRAP;
        }

        console.log(
            '[WASM-JIT-DEBUG] AFTER rx_jit',
            'return=' + String(result)
        );

        /*
         * --------------------------------------------------------------------
         * 23. Estatísticas
         * --------------------------------------------------------------------
         */

        const stats =
            Module.__randomxJitStats ||
            (Module.__randomxJitStats = {
                executions: 0,
                failures: 0,
                traps: 0
            });

        stats.executions++;

        console.log(
            '[WASM-JIT-DEBUG] executions =',
            stats.executions
        );

        console.log(
            '[WASM-JIT-DEBUG] =================================================='
        );

        return ERR.OK;

    } catch (e) {
        /*
         * --------------------------------------------------------------------
         * 24. Catch global
         * --------------------------------------------------------------------
         */

        console.error(
            '[WASM-JIT-FATAL] exceção não tratada'
        );

        console.error(
            '[WASM-JIT-FATAL] name =',
            e && e.name
        );

        console.error(
            '[WASM-JIT-FATAL] message =',
            e && e.message
        );

        console.error(
            '[WASM-JIT-FATAL] stack =',
            e && e.stack
        );

        return ERR.RUNTIME;
    }
});

} // anonymous namespace

/*
 * ============================================================================
 * WasmJit::execute
 * ============================================================================
 */

bool WasmJit::execute(
    uint8_t* regs,
    uint8_t* f,
    uint8_t* e,
    uint8_t* a,
    uint8_t* scratchpad)
{
    /*
     * ------------------------------------------------------------------------
     * Validar módulo
     * ------------------------------------------------------------------------
     */

    if (module_.empty()) {
        printf(
            "[WASM-JIT-ERROR] execute(): módulo WASM vazio\n"
        );

        return false;
    }

    /*
     * ------------------------------------------------------------------------
     * Validar ponteiros
     * ------------------------------------------------------------------------
     */

    if (!regs) {
        printf(
            "[WASM-JIT-ERROR] execute(): regs == nullptr\n"
        );

        return false;
    }

    if (!f) {
        printf(
            "[WASM-JIT-ERROR] execute(): f == nullptr\n"
        );

        return false;
    }

    if (!e) {
        printf(
            "[WASM-JIT-ERROR] execute(): e == nullptr\n"
        );

        return false;
    }

    if (!a) {
        printf(
            "[WASM-JIT-ERROR] execute(): a == nullptr\n"
        );

        return false;
    }

    if (!scratchpad) {
        printf(
            "[WASM-JIT-ERROR] execute(): scratchpad == nullptr\n"
        );

        return false;
    }

    /*
     * ------------------------------------------------------------------------
     * Converter ponteiros para WASM32
     * ------------------------------------------------------------------------
     *
     * O WebAssembly32 utiliza offsets de 32 bits.
     */

    const uintptr_t moduleAddress =
        reinterpret_cast<uintptr_t>(module_.data());

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
     * ------------------------------------------------------------------------
     * Garantir que os ponteiros cabem em WASM32
     * ------------------------------------------------------------------------
     */

    if (moduleAddress > UINT32_MAX) {
        printf(
            "[WASM-JIT-ERROR] module address > UINT32_MAX: %p\n",
            module_.data()
        );

        return false;
    }

    if (regsAddress > UINT32_MAX) {
        printf(
            "[WASM-JIT-ERROR] regs address > UINT32_MAX: %p\n",
            regs
        );

        return false;
    }

    if (fAddress > UINT32_MAX) {
        printf(
            "[WASM-JIT-ERROR] f address > UINT32_MAX: %p\n",
            f
        );

        return false;
    }

    if (eAddress > UINT32_MAX) {
        printf(
            "[WASM-JIT-ERROR] e address > UINT32_MAX: %p\n",
            e
        );

        return false;
    }

    if (aAddress > UINT32_MAX) {
        printf(
            "[WASM-JIT-ERROR] a address > UINT32_MAX: %p\n",
            a
        );

        return false;
    }

    if (scratchpadAddress > UINT32_MAX) {
        printf(
            "[WASM-JIT-ERROR] scratchpad address > UINT32_MAX: %p\n",
            scratchpad
        );

        return false;
    }

    /*
     * ------------------------------------------------------------------------
     * Converter
     * ------------------------------------------------------------------------
     */

    const int modulePtr =
        static_cast<int>(
            static_cast<uint32_t>(moduleAddress)
        );

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

    /*
     * ------------------------------------------------------------------------
     * Log
     * ------------------------------------------------------------------------
     */

    printf(
        "[WASM-JIT-DEBUG] execute: "
        "module=%p "
        "size=%zu "
        "regs=%p "
        "f=%p "
        "e=%p "
        "a=%p "
        "scratchpad=%p\n",
        module_.data(),
        module_.size(),
        regs,
        f,
        e,
        a,
        scratchpad
    );

    printf(
        "[WASM-JIT-DEBUG] WASM32 args: "
        "module=0x%08x "
        "regs=0x%08x "
        "f=0x%08x "
        "e=0x%08x "
        "a=0x%08x "
        "scratchpad=0x%08x\n",
        static_cast<unsigned int>(
            static_cast<uint32_t>(modulePtr)
        ),
        static_cast<unsigned int>(
            static_cast<uint32_t>(regsPtr)
        ),
        static_cast<unsigned int>(
            static_cast<uint32_t>(fPtr)
        ),
        static_cast<unsigned int>(
            static_cast<uint32_t>(ePtr)
        ),
        static_cast<unsigned int>(
            static_cast<uint32_t>(aPtr)
        ),
        static_cast<unsigned int>(
            static_cast<uint32_t>(scratchpadPtr)
        )
    );

    /*
     * ------------------------------------------------------------------------
     * Executar
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

    printf(
        "[WASM-JIT-DEBUG] randomx_wasm_execute_module -> %d\n",
        status
    );

    /*
     * ------------------------------------------------------------------------
     * Resultado
     * ------------------------------------------------------------------------
     */

    if (status != 0) {
        printf(
            "[WASM-JIT-DEBUG] JIT execution SUCCESS\n"
        );

        return true;
    }

    printf(
        "[WASM-JIT-DEBUG] JIT execution FAILED\n"
    );

    return false;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
