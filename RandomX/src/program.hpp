/*
 * Copyright (c) 2018-2019, tevador <tevador@gmail.com>
 * Copyright (c) 2019-2020, SChernykh <schernykh@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RANDOMX_PROGRAM_H
#define RANDOMX_PROGRAM_H

#include <iostream>
#include "instruction.hpp"
#include "virtual_machine.hpp"
#include "intrin_portable.h"

namespace randomx {

	class Program {
	public:
		Program() {
			static_assert(sizeof(programBuffer) == RANDOMX_PROGRAM_SIZE * sizeof(Instruction), "Invalid program buffer size");
			static_assert(sizeof(entropyBuffer) == RANDOMX_ENTROPY_SIZE, "Invalid entropy buffer size");
		}
		void read(const void* data) {
			memcpy(programBuffer, data, sizeof(programBuffer));
			memcpy(entropyBuffer, (uint8_t*)data + sizeof(programBuffer), sizeof(entropyBuffer));
		}
		const Instruction& operator()(int pc) const {
			return programBuffer[pc];
		}
		friend std::ostream& operator<<(std::ostream& os, const Program& p) {
			p.print(os);
			return os;
		}
		uint64_t getEntropy(int i) {
			return load64(&entropyBuffer[i]);
		}
		uint32_t getSize() const {
			return RANDOMX_PROGRAM_SIZE;
		}
	private:
		void print(std::ostream& os) const {
			for (int i = 0; i < RANDOMX_PROGRAM_SIZE; ++i) {
				auto instr = programBuffer[i];
				os << instr;
			}
		}
		Instruction programBuffer[RANDOMX_PROGRAM_SIZE];
		uint8_t entropyBuffer[RANDOMX_ENTROPY_SIZE];
	};

}

#endif // RANDOMX_PROGRAM_H
