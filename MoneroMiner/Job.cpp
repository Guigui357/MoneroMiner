#include "Job.h"
#include "Utils.h"
#include "Config.h"
#include "Types.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>

extern Config config;

Job::Job() : jobId(""), height(0), seedHash(""), difficulty(0), nonceOffset(39), blob() {
    targetHash = {0, 0, 0, 0};
}

Job::Job(const Job& other)
    : jobId(other.jobId)
    , height(other.height)
    , seedHash(other.seedHash)
    , difficulty(other.difficulty)
    , nonceOffset(other.nonceOffset)
    , targetHash(other.targetHash)
    , blob(other.blob)
{
}

Job& Job::operator=(const Job& other) {
    if (this != &other) {
        jobId = other.jobId;
        height = other.height;
        seedHash = other.seedHash;
        difficulty = other.difficulty;
        nonceOffset = other.nonceOffset;
        targetHash = other.targetHash;
        blob = other.blob;
    }
    return *this;
}

Job::Job(const std::string& blobHex, const std::string& id, const std::string& targetHex,
         uint64_t h, const std::string& seed)
    : jobId(id), height(h), seedHash(seed), difficulty(0), nonceOffset(0)
{
    blob = Utils::hexToBytes(blobHex);
    nonceOffset = findNonceOffset();

    const std::vector<uint8_t> targetData = Utils::hexToBytes(targetHex);

    if (targetData.size() == 4) {
        // XMRig-compatible Stratum compact target conversion.
        // The pool target is a little-endian uint32. RandomX share
        // validation compares the most-significant uint64 of the 32-byte
        // hash against this derived uint64 target.
        uint32_t compactTarget = 0;
        for (size_t i = 0; i < 4; ++i) {
            compactTarget |= static_cast<uint32_t>(targetData[i]) << (i * 8);
        }

        if (compactTarget == 0) {
            compactTarget = 1;
        }

        const uint64_t compactDifficulty =
            0xFFFFFFFFULL / static_cast<uint64_t>(compactTarget);

        if (compactDifficulty == 0) {
            difficulty = 0;
            targetHash = {0, 0, 0, 0};
        } else {
            // This is the same formula used by XMRig's Job::setTarget():
            // target64 = UINT64_MAX / (UINT32_MAX / compactTarget).
            const uint64_t target64 =
                0xFFFFFFFFFFFFFFFFULL / compactDifficulty;

            targetHash = {0, 0, 0, target64};
            difficulty = 0xFFFFFFFFFFFFFFFFULL / target64;
        }

        if (config.debugMode) {
            std::stringstream ss;
            ss << "\n=== TARGET CALCULATION ===\n";
            ss << "Compact: 0x" << std::hex << std::setw(8)
               << std::setfill('0') << compactTarget << "\n";
            ss << "Difficulty: " << std::dec << difficulty << "\n";
            ss << "Target64: 0x" << std::hex << std::setw(16)
               << std::setfill('0') << targetHash[3] << "\n";
            ss << "Target (256-bit view): " << getTargetHex();
            Utils::threadSafePrint(ss.str(), true);
        }
    }
    else if (targetData.size() == 8) {
        // Full uint64 Stratum target, little-endian.
        uint64_t target64 = 0;
        for (size_t i = 0; i < 8; ++i) {
            target64 |= static_cast<uint64_t>(targetData[i]) << (i * 8);
        }

        if (target64 == 0) {
            difficulty = 0;
            targetHash = {0, 0, 0, 0};
        } else {
            targetHash = {0, 0, 0, target64};
            difficulty = 0xFFFFFFFFFFFFFFFFULL / target64;
        }
    }
    else if (targetData.size() == 32) {
        // Full 256-bit target support. Keep the little-endian representation.
        // For RandomX Stratum validation, the high uint64 is the effective
        // target used by the pool protocol.
        for (size_t i = 0; i < 4; ++i) {
            uint64_t word = 0;
            for (size_t j = 0; j < 8; ++j) {
                word |= static_cast<uint64_t>(targetData[i * 8 + j]) << (j * 8);
            }
            targetHash[i] = word;
        }

        if (targetHash[3] != 0) {
            difficulty = 0xFFFFFFFFFFFFFFFFULL / targetHash[3];
        } else {
            difficulty = 1;
        }
    }
    else {
        // Invalid target: keep the job non-submit-able rather than silently
        // accepting shares with an unrelated target.
        difficulty = 0;
        targetHash = {0, 0, 0, 0};
    }
}

std::array<uint64_t, 4> Job::difficultyToTarget(uint64_t diff) {
    if (diff == 0) {
        return {0, 0, 0, 0};
    }

    // RandomX Stratum target is a uint64 value stored in the most-significant
    // word of the 256-bit representation used by this project.
    return {0, 0, 0, 0xFFFFFFFFFFFFFFFFULL / diff};
}

bool Job::isValidShare(const std::array<uint64_t, 4>& hashResult) const {
    if (difficulty == 0 || targetHash[3] == 0) {
        return false;
    }

    // XMRig validates RandomX shares from the high uint64 of the 32-byte
    // result. Equality is accepted by the <= comparison.
    return hashResult[3] <= targetHash[3];
}

std::string Job::getTargetHex() const {
    std::stringstream ss;
    for (int wordIdx = 3; wordIdx >= 0; --wordIdx) {
        const uint64_t word = targetHash[static_cast<size_t>(wordIdx)];
        for (int byteIdx = 7; byteIdx >= 0; --byteIdx) {
            const uint8_t byte = static_cast<uint8_t>((word >> (byteIdx * 8)) & 0xFF);
            ss << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte);
        }
    }
    return ss.str();
}

size_t Job::findNonceOffset() const {
    return 39;
}

std::vector<uint8_t> Job::getBlobBytes() const {
    return blob;
}

std::string Job::getJobId() const {
    return jobId;
}

std::string Job::getTarget() const {
    return getTargetHex();
}
