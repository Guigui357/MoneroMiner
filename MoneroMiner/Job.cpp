#include "Job.h"
#include "Utils.h"
#include "Config.h"
#include "Types.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>

extern Config config;

// Default constructor
Job::Job() : jobId(""), height(0), seedHash(""), difficulty(0), nonceOffset(39), blob(), targetBytes{} {
    targetHash = {0, 0, 0, 0};
}

Job::Job(const Job& other) 
    : jobId(other.jobId)
    , height(other.height)
    , seedHash(other.seedHash)
    , difficulty(other.difficulty)
    , nonceOffset(other.nonceOffset)
    , targetHash(other.targetHash)
    , targetBytes(other.targetBytes)
    , blob(other.blob)
{
}

// Copy assignment
Job& Job::operator=(const Job& other) {
    if (this != &other) {
        jobId = other.jobId;
        height = other.height;
        seedHash = other.seedHash;
        difficulty = other.difficulty;
        nonceOffset = other.nonceOffset;
        targetHash = other.targetHash;
        blob = other.blob;
        
        // Remove debug spam - assignment operator is called frequently
    }
    return *this;
}

Job::Job(const std::string& blobHex, const std::string& id, const std::string& targetHex,
         uint64_t h, const std::string& seed)
    : jobId(id), height(h), seedHash(seed), difficulty(0), nonceOffset(0), targetHash{}, targetBytes{}
{
    blob = Utils::hexToBytes(blobHex);
    nonceOffset = findNonceOffset();
    
    std::vector<uint8_t> targetData = Utils::hexToBytes(targetHex);
    
    if (targetData.size() == 4) {
        // Parse 4-byte compact target as little-endian uint32
        uint32_t compactTarget = 0;
        for (size_t i = 0; i < 4; i++) {
            compactTarget |= static_cast<uint32_t>(targetData[i]) << (i * 8);
        }
        
        if (compactTarget == 0) compactTarget = 1;
        
        // Calculate pool difficulty
        difficulty = static_cast<uint64_t>(0xFFFFFFFFULL) / static_cast<uint64_t>(compactTarget);
        
        // Calculate 256-bit target using TRUE division: (2^256 - 1) / difficulty
        uint256_t maxValue = uint256_t::maximum();
        uint256_t target256 = maxValue / difficulty;
        
        // Store result
        targetHash[0] = target256.data[0];
        targetHash[1] = target256.data[1];
        targetHash[2] = target256.data[2];
        targetHash[3] = target256.data[3];
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < 8; ++j) {
                targetBytes[i * 8 + j] = static_cast<uint8_t>((targetHash[i] >> (j * 8)) & 0xFF);
            }
        }
        
        if (config.debugMode) {
            std::stringstream ss;
            ss << "\n=== TARGET CALCULATION ===\n";
            ss << "Compact: 0x" << std::hex << std::setw(8) << std::setfill('0') << compactTarget << "\n";
            ss << "Difficulty: " << std::dec << difficulty << "\n";
            ss << "Target (256-bit): " << target256.toHex();
            Utils::threadSafePrint(ss.str(), true);
        }
        
    } else if (targetData.size() == 32) {
        // Pool sent full 256-bit target (rare, but handle it)
        // Parse as little-endian 256-bit value
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < 8; ++j) {
                targetHash[i] |= static_cast<uint64_t>(targetData[i * 8 + j]) << (j * 8);
            }
        }
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < 8; ++j) {
                targetBytes[i * 8 + j] = static_cast<uint8_t>((targetHash[i] >> (j * 8)) & 0xFF);
            }
        }
        
        // Calculate difficulty from target
        uint256_t targetValue(targetData.data());
        if (targetValue > uint256_t()) {
            difficulty = static_cast<uint64_t>((uint256_t::maximum() / targetValue).data[0]);
            if (difficulty == 0) {
                difficulty = 1;
            }
        } else {
            difficulty = 1;
        }
        
    } else {
        // Invalid target size - set to maximum difficulty (hardest target)
        difficulty = 1;
        targetHash = {0xFFFFFFFFFFFFFFFFULL, 0, 0, 0};
        targetBytes.fill(0);
        targetBytes[31] = 0xFF;
    }
}

std::array<uint64_t, 4> Job::difficultyToTarget(uint64_t difficulty) {
    /*
     * Convert pool difficulty to 256-bit comparison target
     * ====================================================
     * 
     * Formula: target = 0xFFFFFFFFFFFFFFFF / difficulty
     * 
     * This gives us the maximum hash value that meets the difficulty.
     * Any RandomX hash output < target is a valid share.
     */
    
    std::array<uint64_t, 4> target = {0, 0, 0, 0};
    
    if (difficulty == 0) difficulty = 1;
    
    // Calculate maximum hash value for this difficulty
    uint64_t maxHash = 0xFFFFFFFFFFFFFFFFULL / difficulty;
    
    // Store in first word (little-endian)
    target[0] = maxHash;
    // Rest are zero (difficulty only affects first 64 bits for typical pool mining)
    
    return target;
}

bool Job::isValidShare(const std::array<uint64_t, 4>& hashResult) const {
    /*
     * Compare 256-bit hash against target (little-endian comparison)
     * =============================================================
     * 
     * Compare from MSW to LSW (index 3 down to 0).
     * Valid share: hashResult < targetHash
     * 
     * This matches how xmrig/xmr-stak perform validation.
     */
    
    // Compare from most significant to least significant
    for (int i = 3; i >= 0; i--) {
        if (hashResult[i] < targetHash[i]) {
            return true;  // Hash is definitely less than target
        }
        if (hashResult[i] > targetHash[i]) {
            return false; // Hash is definitely greater than target
        }
        // If equal, continue to next word
    }
    
    // All words equal - hash == target, which is valid (hash <= target)
    return true;
}

std::string Job::getTargetHex() const {
    std::stringstream ss;
    // Display in big-endian order (MSW first) to match comparison display
    for (int wordIdx = 3; wordIdx >= 0; wordIdx--) {
        uint64_t word = targetHash[wordIdx];
        // Display each word's bytes in big-endian order
        for (int byteIdx = 7; byteIdx >= 0; byteIdx--) {
            uint8_t byte = (word >> (byteIdx * 8)) & 0xFF;
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
    }
    return ss.str();
}

size_t Job::findNonceOffset() const {
    // Monero pool mining: nonce is ALWAYS at byte 39 (0-indexed)
    return 39;
}

const std::vector<uint8_t>& Job::getBlobBytes() const {
    return blob;
}

const std::array<uint8_t, 32>& Job::getTargetBytes() const {
    return targetBytes;
}

std::string Job::getJobId() const {
    return jobId;
}

std::string Job::getTarget() const {
    return getTargetHex();
}