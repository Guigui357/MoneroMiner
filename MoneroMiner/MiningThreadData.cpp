#include "MiningThreadData.h"
#include "RandomXManager.h"
#include "Utils.h"
#include "Config.h"
#include "Globals.h"
#include "Types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <string>

namespace PoolClient {
extern std::string currentSeedHash;
}

MiningThreadData::MiningThreadData(int id) : threadId(id) {}

MiningThreadData::~MiningThreadData() {
    RandomXManager::cleanupVM(threadId);
    vm = nullptr;
}

bool MiningThreadData::initializeVM() {
    const std::string& seedHash = PoolClient::currentSeedHash;

    if (seedHash.empty()) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": current job has no seed hash", true);
        return false;
    }

    Utils::threadSafePrint(
        "[RandomX] Thread " + std::to_string(threadId) +
        ": ensuring manager is initialized for seed " + seedHash, true);

    if (!RandomXManager::isInitialized() ||
        RandomXManager::getCurrentSeedHash() != seedHash) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": initializing RandomX manager", true);

        if (!RandomXManager::initialize(seedHash)) {
            Utils::threadSafePrint(
                "[RandomX] Thread " + std::to_string(threadId) +
                ": RandomXManager::initialize() failed", true);
            return false;
        }
    }

    if (vm != nullptr)
        return true;

    Utils::threadSafePrint(
        "[WASM-DEBUG] >>> initializeVM(" + std::to_string(threadId) + ") ENTROU",
        true);

    if (!RandomXManager::initializeVM(threadId)) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": initializeVM() failed", true);
        return false;
    }

    vm = RandomXManager::getVM(threadId);

    if (!vm) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": VM lookup returned NULL", true);
        return false;
    }

    Utils::threadSafePrint(
        "[RandomX] Thread " + std::to_string(threadId) + ": VM ready", true);

    return true;
}

bool MiningThreadData::calculateHash(const std::vector<uint8_t>& input, uint64_t nonce) {
    bool result = RandomXManager::calculateHashForThread(threadId, input, nonce);
    incrementHashCount();
    return result;
}

bool MiningThreadData::calculateHashAndCheckTarget(
    const std::vector<uint8_t>& blob,
    const std::array<uint64_t, 4>& targetWords,
    std::vector<uint8_t>& hashOut)
{
    if (!vm) {
        Utils::threadSafePrint(
            "[RandomX] T" + std::to_string(threadId) +
            ": calculateHash called without VM", true);
        return false;
    }

    if (blob.empty() || blob.size() > 128) {
        Utils::threadSafePrint(
            "[RandomX] T" + std::to_string(threadId) +
            ": invalid blob size " + std::to_string(blob.size()), true);
        return false;
    }

    if (hashOut.size() < RANDOMX_HASH_SIZE)
        hashOut.resize(RANDOMX_HASH_SIZE);

    // RandomX writes the complete 32-byte result.
    randomx_calculate_hash(vm, blob.data(), blob.size(), hashOut.data());

    hashCount.fetch_add(1, std::memory_order_relaxed);
    totalHashes.fetch_add(1, std::memory_order_relaxed);

    // XMRig's RandomX share validation uses the high uint64 of the 32-byte
    // result. The Stratum target is likewise represented as a uint64.
    uint64_t hashHigh = 0;
    for (size_t j = 0; j < 8; ++j) {
        hashHigh |= static_cast<uint64_t>(hashOut[24 + j]) << (j * 8);
    }

    const uint64_t targetHigh = targetWords[3];
    const bool isValid = targetHigh != 0 && hashHigh <= targetHigh;

    if (config.debugMode && (isValid || (totalHashes % 10000 == 0))) {
        uint256_t hashValue;
        hashValue.data = {0, 0, 0, hashHigh};
        uint256_t targetValue;
        targetValue.data = {0, 0, 0, targetHigh};

        std::stringstream ss;
        ss << "[T" << threadId << " PoW @ " << totalHashes << " hashes]\n";
        ss << "  HashHigh:   0x" << std::hex << std::setw(16)
           << std::setfill('0') << hashHigh << "\n";
        ss << "  TargetHigh: 0x" << std::hex << std::setw(16)
           << std::setfill('0') << targetHigh << "\n";
        ss << "  Hash:   " << hashValue.toHex() << "\n";
        ss << "  Target: " << targetValue.toHex() << "\n";
        ss << "  Result: " << (isValid ? "VALID SHARE FOUND!" : "does not meet target");
        if (isValid)
            ss << "\n  >>> SUBMITTING SHARE <<<";
        Utils::threadSafePrint(ss.str(), true);
    }

    return isValid;
}

bool MiningThreadData::calculateHashAndCheckTarget(
    const std::vector<uint8_t>& blob,
    const std::vector<uint8_t>& targetBytes,
    std::vector<uint8_t>& hashOut)
{
    if (targetBytes.size() != 32) {
        Utils::threadSafePrint(
            "[RandomX] T" + std::to_string(threadId) +
            ": invalid target size " + std::to_string(targetBytes.size()), true);
        return false;
    }

    std::array<uint64_t, 4> targetWords{};
    for (size_t i = 0; i < 4; ++i) {
        uint64_t word = 0;
        for (size_t j = 0; j < 8; ++j) {
            word |= static_cast<uint64_t>(targetBytes[i * 8 + j]) << (j * 8);
        }
        targetWords[i] = word;
    }

    return calculateHashAndCheckTarget(blob, targetWords, hashOut);
}
