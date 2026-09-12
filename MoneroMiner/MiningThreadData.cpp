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
    const std::array<uint8_t, 32>& target,
    std::array<uint8_t, 32>& hashOut)
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

    randomx_calculate_hash(vm, blob.data(), blob.size(), hashOut.data());

    bool isValid = false;
    for (size_t i = RANDOMX_HASH_SIZE - 1;; --i) {
        if (hashOut[i] < target[i]) {
            isValid = true;
            break;
        }
        if (hashOut[i] > target[i]) {
            break;
        }
        if (i == 0) {
            break;
        }
    }

    if (config.debugMode && isValid) {
        std::stringstream ss;
        ss << "[T" << threadId << "] Valid share candidate\n  Hash:   ";
        for (uint8_t byte : hashOut) {
            ss << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte);
        }
        ss << "\n  Target: ";
        for (uint8_t byte : target) {
            ss << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte);
        }
        Utils::threadSafePrint(ss.str(), true);
    }

    return isValid;
}
