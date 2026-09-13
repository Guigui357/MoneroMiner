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
		// The WASM JIT accepts only instructions for which the generated module
		// preserves the interpreter semantics. Unsupported FP-memory/control
		// instructions automatically use the canonical interpreter.
		wasmJitReady = wasmJit.compile(program);
#endif

		uint32_t spAddr0 = mem.mx;
		uint32_t spAddr1 = mem.ma;

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

        // WASM: FORCAR EXECUCAO PELO WASM-JIT.
        if (!wasmJitReady) {
            std::cerr << "[WASM-JIT] ERRO: JIT nao esta pronto!" << std::endl;
        }

        std::cout << "[WASM-JIT] EXECUTANDO rx_jit..." << std::endl;

        const bool jitResult = wasmJit.execute(
            reinterpret_cast<uint8_t*>(nreg.r),
            reinterpret_cast<uint8_t*>(nreg.f),
            reinterpret_cast<uint8_t*>(nreg.e),
            reinterpret_cast<uint8_t*>(nreg.a),
            scratchpad
        );

        if (!jitResult) {
            std::cerr << "[WASM-JIT] FALHA: execucao do rx_jit!" << std::endl;
            return;
        }

        std::cout << "[WASM-JIT] rx_jit executado com sucesso." << std::endl;

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
