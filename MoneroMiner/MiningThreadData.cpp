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

MiningThreadData::MiningThreadData(int id) : threadId(id) {
}

MiningThreadData::~MiningThreadData() {
    RandomXManager::cleanupVM(threadId);
    vm = nullptr;
}

bool MiningThreadData::initializeVM() {
    if (vm != nullptr) {
        return true;
    }

    if (!RandomXManager::isInitialized()) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": manager not initialized",
            true
        );
        return false;
    }

    Utils::threadSafePrint(
        "[WASM-DEBUG] >>> initializeVM(" + std::to_string(threadId) + ") ENTROU",
        true
    );

    if (!RandomXManager::initializeVM(threadId)) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": initializeVM() failed",
            true
        );
        return false;
    }

    vm = RandomXManager::getVM(threadId);

    if (!vm) {
        Utils::threadSafePrint(
            "[RandomX] Thread " + std::to_string(threadId) +
            ": VM lookup returned NULL",
            true
        );
        return false;
    }

    Utils::threadSafePrint(
        "[RandomX] Thread " + std::to_string(threadId) + ": VM ready",
        true
    );

    return true;
}

bool MiningThreadData::calculateHash(const std::vector<uint8_t>& input, uint64_t nonce) {
    bool result = RandomXManager::calculateHashForThread(threadId, input, nonce);
    incrementHashCount();
    return result;
}

bool MiningThreadData::calculateHashAndCheckTarget(
    const std::vector<uint8_t>& blob,
    const std::vector<uint8_t>& targetBytes,
    std::vector<uint8_t>& hashOut)
{
    if (!vm) {
        Utils::threadSafePrint(
            "[RandomX] T" + std::to_string(threadId) +
            ": calculateHash called without VM",
            true
        );
        return false;
    }

    if (blob.empty() || blob.size() > 128) {
        Utils::threadSafePrint(
            "[RandomX] T" + std::to_string(threadId) +
            ": invalid blob size " + std::to_string(blob.size()),
            true
        );
        return false;
    }

    if (targetBytes.size() != 32) {
        Utils::threadSafePrint(
            "[RandomX] T" + std::to_string(threadId) +
            ": invalid target size " + std::to_string(targetBytes.size()),
            true
        );
        return false;
    }

    if (hashOut.size() < RANDOMX_HASH_SIZE) {
        hashOut.resize(RANDOMX_HASH_SIZE);
    }

    // Keep the VM owned exclusively by this mining worker.
    // Do not access another worker's VM or the manager's mutable lastHash here.
    randomx_calculate_hash(
        vm,
        blob.data(),
        blob.size(),
        hashOut.data()
    );

    totalHashes++;

    uint256_t hashValue(hashOut.data());
    uint256_t targetValue(targetBytes.data());
    bool isValid = hashValue < targetValue;

    if (config.debugMode && (isValid || (totalHashes % 10000 == 0))) {
        std::stringstream ss;
        ss << "[T" << threadId << " PoW @ " << totalHashes << " hashes]\n";
        ss << "  Hash:   " << hashValue.toHex() << "\n";
        ss << "  Target: " << targetValue.toHex() << "\n";
        ss << "  Result: " << (isValid ? "VALID SHARE FOUND!" : "does not meet target");

        if (isValid) {
            ss << "\n  >>> SUBMITTING SHARE <<<";
        }

        Utils::threadSafePrint(ss.str(), true);
    }

    return isValid;
}
