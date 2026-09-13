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
 * * Neither the name of the copyright holder nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
 */

#ifndef RANDOMX_PROGRAM_H
#define RANDOMX_PROGRAM_H

#include <cstdint>
#include <iostream>

#include "common.hpp"
#include "instruction.hpp"
#include "blake2/endian.h"

namespace randomx {

	struct ProgramConfiguration {
		uint64_t eMask[2];
		uint32_t readReg0, readReg1, readReg2, readReg3;
	};

	class Program {
	public:
		Program() {
			static_assert(sizeof(programBuffer) == RANDOMX_PROGRAM_SIZE * sizeof(Instruction), "Invalid program buffer size");
			static_assert(sizeof(entropyBuffer) == 16 * sizeof(uint64_t), "Invalid entropy buffer size");
		}

		void read(const void* data) {
			memcpy(programBuffer, data, sizeof(programBuffer));
			memcpy(entropyBuffer, (const uint8_t*)data + sizeof(programBuffer), sizeof(entropyBuffer));
		}

		Instruction& operator()(int pc) {
			return programBuffer[pc];
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

		uint64_t entropyBuffer[16];
		Instruction programBuffer[RANDOMX_PROGRAM_SIZE];
	};

	static_assert(sizeof(Program) % 64 == 0, "Invalid size of class randomx::Program");

}

#endif // RANDOMX_PROGRAM_H
