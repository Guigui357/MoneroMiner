#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace MiningHotPath {

// Writes the RandomX nonce without relying on host endianness.
inline void writeNonceLE(std::vector<uint8_t>& blob, size_t offset, uint32_t nonce) {
    blob[offset + 0] = static_cast<uint8_t>(nonce);
    blob[offset + 1] = static_cast<uint8_t>(nonce >> 8);
    blob[offset + 2] = static_cast<uint8_t>(nonce >> 16);
    blob[offset + 3] = static_cast<uint8_t>(nonce >> 24);
}

// Compare a 256-bit little-endian hash against the Job target.
inline bool hashBelowTarget(const uint8_t* hash, const std::array<uint64_t, 4>& target) {
    for (int i = 3; i >= 0; --i) {
        uint64_t word = 0;
        const size_t base = static_cast<size_t>(i) * 8;
        for (size_t j = 0; j < 8; ++j)
            word |= static_cast<uint64_t>(hash[base + j]) << (j * 8);

        if (word < target[static_cast<size_t>(i)]) return true;
        if (word > target[static_cast<size_t>(i)]) return false;
    }
    return false;
}

} // namespace MiningHotPath
