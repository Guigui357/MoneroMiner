/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>

All rights reserved.
*/

#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <cfloat>
#include "vm_interpreted.hpp"
#include "dataset.hpp"
#include "intrin_portable.h"
#include "reciprocal.h"

namespace randomx {

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::setDataset(randomx_dataset* dataset) {
		datasetPtr = dataset;
		mem.memory = dataset->memory;
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::run(void* seed) {
		VmBase<Allocator, softAes>::generateProgram(seed);
		randomx_vm::initialize();
		execute();
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::execute() {

		NativeRegisterFile nreg;

		for(unsigned i = 0; i < RegisterCountFlt; ++i)
			nreg.a[i] = rx_load_vec_f128(&reg.a[i].lo);

		compileProgram(program, bytecode, nreg);

#ifdef __EMSCRIPTEN__
        // The custom WASM JIT is independent of RANDOMX_FLAG_JIT.
        // RANDOMX_FLAG_JIT refers to the native x86/ARM JIT and must
        // remain disabled on WebAssembly. The generated WASM module
        // executes the RandomX program directly against this register file.
        if (!wasmJitDisabled) {
            wasmJitReady = wasmJit.compile(program);
            if (!wasmJitReady) {
                wasmJitDisabled = true;
                std::cerr << "[WASM-JIT] compile failed; disabling JIT and using interpreter" << std::endl;
            }
            else {
                std::cerr << "[WASM-JIT] custom JIT ready; hashing through generated WASM" << std::endl;
            }
        }
#endif

		uint32_t spAddr0 = mem.mx;
		uint32_t spAddr1 = mem.ma;

#ifdef __EMSCRIPTEN__
        // Once a generated module fails at runtime, never enter it again
        // for this VM. This prevents repeated WebAssembly traps on mobile.
        const bool wasmJitUsable = wasmJitReady && !wasmJitDisabled;
#endif

		for(unsigned ic = 0; ic < RANDOMX_PROGRAM_ITERATIONS; ++ic) {
			uint64_t spMix = nreg.r[config.readReg0] ^ nreg.r[config.readReg1];
			spAddr0 ^= spMix;
			spAddr0 &= ScratchpadL3Mask64;
			spAddr1 ^= spMix >> 32;
			spAddr1 &= ScratchpadL3Mask64;
			
			for (unsigned i = 0; i < RegistersCount; ++i)
				nreg.r[i] ^= load64(scratchpad + spAddr0 + 8 * i);

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				nreg.f[i] = rx_cvt_packed_int_vec_f128(scratchpad + spAddr1 + 8 * i);

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				nreg.e[i] = maskRegisterExponentMantissa(config, rx_cvt_packed_int_vec_f128(scratchpad + spAddr1 + 8 * (RegisterCountFlt + i)));

#ifdef __EMSCRIPTEN__

        bool executed = false;

        if (wasmJitUsable) {
            executed = wasmJit.execute(
                reinterpret_cast<uint8_t*>(nreg.r),
                reinterpret_cast<uint8_t*>(nreg.f),
                reinterpret_cast<uint8_t*>(nreg.e),
                reinterpret_cast<uint8_t*>(nreg.a),
                scratchpad
            );

            if (!executed) {
                // Disable permanently for this VM. The interpreter remains
                // the correctness-preserving fallback for the current hash.
                wasmJitDisabled = true;
                std::cerr << "[WASM-JIT] execution failed; disabling JIT for this VM and using interpreter" << std::endl;
            }
        }

        if (!executed) {
            executeBytecode(bytecode, scratchpad, config);
        }
			
#else
        executeBytecode(bytecode, scratchpad, config);

#endif
			mem.mx ^= nreg.r[config.readReg2] ^ nreg.r[config.readReg3];
			mem.mx &= CacheLineAlignMask;
			datasetPrefetch(datasetOffset + mem.mx);
			datasetRead(datasetOffset + mem.ma, nreg.r);
			std::swap(mem.mx, mem.ma);

			for (unsigned i = 0; i < RegistersCount; ++i)
				store64(scratchpad + spAddr1 + 8 * i, nreg.r[i]);

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				nreg.f[i] = rx_xor_vec_f128(nreg.f[i], nreg.e[i]);

			for (unsigned i = 0; i < RegisterCountFlt; ++i)
				rx_store_vec_f128((double*)(scratchpad + spAddr0 + 16 * i), nreg.f[i]);

			spAddr0 = 0;
			spAddr1 = 0;
		}

		for (unsigned i = 0; i < RegistersCount; ++i)
			store64(&reg.r[i], nreg.r[i]);

		for (unsigned i = 0; i < RegisterCountFlt; ++i)
			rx_store_vec_f128(&reg.f[i].lo, nreg.f[i]);

		for (unsigned i = 0; i < RegisterCountFlt; ++i)
			rx_store_vec_f128(&reg.e[i].lo, nreg.e[i]);
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::datasetRead(uint64_t address, int_reg_t(&r)[RegistersCount]) {
		uint64_t* datasetLine = (uint64_t*)(mem.memory + address);
		for (int i = 0; i < RegistersCount; ++i)
			r[i] ^= datasetLine[i];
	}

	template<class Allocator, bool softAes>
	void InterpretedVm<Allocator, softAes>::datasetPrefetch(uint64_t address) {
		rx_prefetch_nta(mem.memory + address);
	}

	template class InterpretedVm<AlignedAllocator<CacheLineSize>, false>;
	template class InterpretedVm<AlignedAllocator<CacheLineSize>, true>;
	template class InterpretedVm<LargePageAllocator, false>;
	template class InterpretedVm<LargePageAllocator, true>;
}