#include "RandomXManager.h"
#include "Config.h"
#include "Utils.h"
#include "Globals.h"
#include "Platform.h"

#include <fstream>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <memory>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <random>
#include <thread>
#include <cstring>
#include <filesystem>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

static constexpr size_t MAX_BLOB_SIZE = 128;

int RandomXManager::flags = RANDOMX_FLAG_DEFAULT;
static int cacheAllocFlags = RANDOMX_FLAG_DEFAULT;

std::shared_mutex RandomXManager::vmMutex;
std::mutex RandomXManager::initMutex;
std::mutex RandomXManager::hashMutex;
std::mutex RandomXManager::cacheMutex;
std::mutex RandomXManager::seedHashMutex;
std::mutex RandomXManager::targetMutex;

std::unordered_map<int, randomx_vm*> RandomXManager::vms;
randomx_cache* RandomXManager::cache = nullptr;
randomx_dataset* RandomXManager::dataset = nullptr;
std::string RandomXManager::currentSeedHash;
bool RandomXManager::initialized = false;
bool RandomXManager::useLightMode = false;
std::vector<uint8_t> RandomXManager::lastHash;
double RandomXManager::currentDifficulty = 0.0;
uint256_t RandomXManager::expandedTarget;

bool RandomXManager::initializeCache(const std::string& seedHash)
{
    Utils::threadSafePrint("[RandomX] Inicializando cache...", true);
    if (seedHash.empty()) return false;
    if (seedHash.size() != 64) return false;

    std::vector<uint8_t> seedBytes;
    try {
        seedBytes.reserve(32);
        for (size_t i = 0; i < seedHash.size(); i += 2) {
            unsigned int byteValue = 0;
            std::stringstream ss;
            ss << std::hex << seedHash.substr(i, 2);
            ss >> byteValue;
            seedBytes.push_back(static_cast<uint8_t>(byteValue));
        }
    } catch (...) {
        return false;
    }
    if (seedBytes.size() != 32) return false;

    int detectedFlags = randomx_get_flags();
    Utils::threadSafePrint("[RandomX] CPU flags detectadas: 0x" + Utils::formatHex(static_cast<uint64_t>(detectedFlags), 8), true);

#ifdef __EMSCRIPTEN__
    useLightMode = true;
    cacheAllocFlags = RANDOMX_FLAG_DEFAULT;
    flags = RANDOMX_FLAG_DEFAULT;
    Utils::threadSafePrint("[WASM] RandomX LIGHT MODE", true);
    Utils::threadSafePrint("[WASM] FULL_MEM desativado", true);
    Utils::threadSafePrint("[WASM] LARGE_PAGES desativado", true);
    Utils::threadSafePrint("[WASM] JIT desativado para compatibilidade", true);
#else
    useLightMode = false;
    cacheAllocFlags = detectedFlags & ~RANDOMX_FLAG_FULL_MEM;
    flags = detectedFlags | RANDOMX_FLAG_FULL_MEM | RANDOMX_FLAG_JIT;
    cacheAllocFlags |= RANDOMX_FLAG_JIT;
    if (Platform::hasHugePagesSupport()) {
        flags |= RANDOMX_FLAG_LARGE_PAGES;
        cacheAllocFlags |= RANDOMX_FLAG_LARGE_PAGES;
        Utils::threadSafePrint("[RandomX] Large pages habilitadas", true);
    } else {
        Utils::threadSafePrint("[RandomX] Large pages indisponíveis", true);
    }
#endif

    if (cache != nullptr) {
        randomx_release_cache(cache);
        cache = nullptr;
    }
    cache = randomx_alloc_cache(static_cast<randomx_flags>(cacheAllocFlags));
    if (cache == nullptr) return false;
    randomx_init_cache(cache, seedBytes.data(), seedBytes.size());
    Utils::threadSafePrint("[RandomX] Cache RandomX inicializado", true);
    currentSeedHash = seedHash;
    return true;
}

bool RandomXManager::createDataset()
{
#ifdef __EMSCRIPTEN__
    Utils::threadSafePrint("[WASM] createDataset() ignorado: usando LIGHT MODE", true);
    return false;
#else
    if (!cache) return false;
    if (dataset) { randomx_release_dataset(dataset); dataset = nullptr; }
    dataset = randomx_alloc_dataset(static_cast<randomx_flags>(flags));
    if (!dataset) {
        flags = RANDOMX_FLAG_FULL_MEM;
        dataset = randomx_alloc_dataset(RANDOMX_FLAG_FULL_MEM);
        if (!dataset) return false;
    }
    unsigned long itemCount = randomx_dataset_item_count();
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 1;
    if (numThreads > 1) numThreads = (std::max)(1u, numThreads - 1u);
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    unsigned long itemsPerThread = itemCount / numThreads;
    for (unsigned int t = 0; t < numThreads; ++t) {
        unsigned long startIndex = t * itemsPerThread;
        unsigned long count = (t == numThreads - 1) ? (itemCount - startIndex) : itemsPerThread;
        threads.emplace_back([startIndex, count]() { randomx_init_dataset(dataset, cache, startIndex, count); });
    }
    for (auto& thread : threads) thread.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    Utils::threadSafePrint("Dataset initialized in " + std::to_string(duration.count() / 1000.0) + " seconds", true);
    return true;
#endif
}

bool RandomXManager::initialize(const std::string& seedHash)
{
    std::lock_guard<std::mutex> lock(initMutex);
    Utils::threadSafePrint("[WASM-DEBUG] >>> RandomXManager::initialize() ENTROU", true);
    Utils::threadSafePrint("[WASM-DEBUG] seedHash = " + seedHash, true);
    if (seedHash.empty() || seedHash.size() != 64) return false;
    if (seedHash == currentSeedHash && cache != nullptr && initialized) return true;
    if (!initializeCache(seedHash)) { initialized = false; return false; }
#ifdef __EMSCRIPTEN__
    useLightMode = true;
    flags = RANDOMX_FLAG_DEFAULT;
#else
    if (!useLightMode) {
        std::string datasetFileName = "randomx_dataset_" + seedHash.substr(0, 16) + ".bin";
        bool loadedDataset = false;
        if (std::filesystem::exists(datasetFileName)) {
            size_t fileSize = std::filesystem::file_size(datasetFileName);
            unsigned long itemCount = randomx_dataset_item_count();
            size_t expectedMinSize = static_cast<size_t>(itemCount) * RANDOMX_DATASET_ITEM_SIZE;
            if (fileSize >= expectedMinSize) loadedDataset = loadDataset(datasetFileName);
            else std::filesystem::remove(datasetFileName);
        }
        if (!loadedDataset) {
            if (!createDataset()) { useLightMode = true; flags = cacheAllocFlags; }
            else saveDataset(datasetFileName);
        }
    }
#endif
    currentSeedHash = seedHash;
    initialized = true;
    Utils::threadSafePrint("[WASM-DEBUG] initialized = true", true);
    Utils::threadSafePrint("=== RANDOMX READY ===", true);
    return true;
}

bool RandomXManager::createVM(int threadId)
{
    std::unique_lock<std::shared_mutex> lock(vmMutex);
    if (!initialized || cache == nullptr) return false;
    auto existing = vms.find(threadId);
    if (existing != vms.end() && existing->second != nullptr) return true;
#ifdef __EMSCRIPTEN__
    randomx_vm* vm = randomx_create_vm(RANDOMX_FLAG_DEFAULT, cache, nullptr);
#else
    if (!useLightMode && dataset == nullptr) return false;
    randomx_vm* vm = randomx_create_vm(static_cast<randomx_flags>(flags), cache, useLightMode ? nullptr : dataset);
#endif
    if (vm == nullptr) return false;
    vms[threadId] = vm;
    return true;
}

bool RandomXManager::initializeVM(int threadId)
{
    return createVM(threadId);
}

randomx_vm* RandomXManager::getVM(int threadId)
{
    std::shared_lock<std::shared_mutex> lock(vmMutex);
    auto it = vms.find(threadId);
    return it != vms.end() ? it->second : nullptr;
}

bool RandomXManager::loadDataset(const std::string& filename)
{
#ifdef __EMSCRIPTEN__
    return false;
#else
    unsigned long itemCount = randomx_dataset_item_count();
    size_t actualDatasetSize = static_cast<size_t>(itemCount) * RANDOMX_DATASET_ITEM_SIZE;
    if (!dataset) {
        dataset = randomx_alloc_dataset(static_cast<randomx_flags>(flags));
        if (!dataset) return false;
    }
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;
    void* datasetMemory = randomx_get_dataset_memory(dataset);
    if (!datasetMemory) { file.close(); return false; }
    file.read(reinterpret_cast<char*>(datasetMemory), actualDatasetSize);
    bool success = file.good() || file.eof();
    file.close();
    return success;
#endif
}

bool RandomXManager::saveDataset(const std::string& filename)
{
#ifdef __EMSCRIPTEN__
    return false;
#else
    if (!dataset) return false;
    unsigned long itemCount = randomx_dataset_item_count();
    size_t actualDatasetSize = static_cast<size_t>(itemCount) * RANDOMX_DATASET_ITEM_SIZE;
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;
    void* datasetMemory = randomx_get_dataset_memory(dataset);
    if (!datasetMemory) { file.close(); return false; }
    file.write(reinterpret_cast<const char*>(datasetMemory), actualDatasetSize);
    file.close();
    return true;
#endif
}

void RandomXManager::cleanupVM(int threadId)
{
    std::unique_lock<std::shared_mutex> lock(vmMutex);
    auto it = vms.find(threadId);
    if (it != vms.end() && it->second) { randomx_destroy_vm(it->second); vms.erase(it); }
}

void RandomXManager::destroyVM(randomx_vm* vm)
{
    if (!vm) return;
    std::unique_lock<std::shared_mutex> lock(vmMutex);
    for (auto it = vms.begin(); it != vms.end(); ++it) {
        if (it->second == vm) { randomx_destroy_vm(vm); vms.erase(it); break; }
    }
}

void RandomXManager::cleanup()
{
    std::lock_guard<std::mutex> lock(initMutex);
    {
        std::unique_lock<std::shared_mutex> vmLock(vmMutex);
        for (auto& [threadId, vm] : vms) { (void)threadId; if (vm) randomx_destroy_vm(vm); }
        vms.clear();
    }
    if (cache) { randomx_release_cache(cache); cache = nullptr; }
#ifndef __EMSCRIPTEN__
    if (dataset) { randomx_release_dataset(dataset); dataset = nullptr; }
#else
    dataset = nullptr;
#endif
    initialized = false;
    currentSeedHash.clear();
}

bool RandomXManager::setTargetAndDifficulty(const std::string& targetHex)
{
    if (targetHex.length() != 8) return false;
    try {
        std::lock_guard<std::mutex> lock(targetMutex);
        std::vector<uint8_t> targetBytes = Utils::hexToBytes(targetHex);
        if (targetBytes.size() < 4) return false;
        uint32_t compactTarget = 0;
        for (size_t i = 0; i < 4; ++i)
            compactTarget |= static_cast<uint32_t>(targetBytes[i]) << (i * 8);
        if (compactTarget == 0) compactTarget = 1;
        currentDifficulty = static_cast<double>(0xFFFFFFFFULL) / static_cast<double>(compactTarget);
        uint64_t diff64 = static_cast<uint64_t>(currentDifficulty);
        if (diff64 == 0) diff64 = 1;

        // Single canonical representation: the effective RandomX/Stratum
        // target is the high uint64 (bytes 24..31) of the 32-byte hash.
        expandedTarget.data[0] = 0;
        expandedTarget.data[1] = 0;
        expandedTarget.data[2] = 0;
        expandedTarget.data[3] = 0xFFFFFFFFFFFFFFFFULL / diff64;

        if (config.debugMode) {
            std::stringstream ss;
            ss << "[TARGET] 0x" << std::hex << compactTarget
               << " -> Diff:" << std::dec << diff64
               << " -> Target[3]=0x" << std::hex << std::setw(16)
               << std::setfill('0') << expandedTarget.data[3];
            Utils::threadSafePrint(ss.str(), true);
        }
        return true;
    } catch (const std::exception& e) {
        Utils::threadSafePrint("Error parsing target: " + std::string(e.what()), true);
        return false;
    }
}

bool RandomXManager::checkTarget(const uint8_t* hash)
{
    if (!hash) return false;

    uint64_t hashHigh = 0;
    for (size_t i = 0; i < 8; ++i)
        hashHigh |= static_cast<uint64_t>(hash[24 + i]) << (i * 8);

    uint64_t targetHigh;
    {
        std::lock_guard<std::mutex> lock(targetMutex);
        targetHigh = expandedTarget.data[3];
    }

    if (targetHigh == 0 || hashHigh > targetHigh) return false;

    {
        std::lock_guard<std::mutex> lock(hashMutex);
        lastHash.assign(hash, hash + RANDOMX_HASH_SIZE);
    }

    if (config.debugMode) {
        std::stringstream ss;
        ss << "\n*** VALID SHARE FOUND ***\n"
           << "HashHigh:   0x" << std::hex << std::setw(16) << std::setfill('0') << hashHigh
           << "\nTargetHigh: 0x" << std::hex << std::setw(16) << std::setfill('0') << targetHigh
           << "\nFull hash: " << Utils::bytesToHex(hash, 32);
        Utils::threadSafePrint(ss.str(), true);
    }
    return true;
}

std::vector<uint8_t> RandomXManager::getLastHash()
{
    std::lock_guard<std::mutex> lock(hashMutex);
    return lastHash;
}

std::string RandomXManager::getLastHashHex()
{
    std::lock_guard<std::mutex> lock(hashMutex);
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : lastHash) ss << std::setw(2) << static_cast<int>(byte);
    return ss.str();
}

randomx_dataset* RandomXManager::getDataset() { return dataset; }
randomx_cache* RandomXManager::getCache() { return cache; }
randomx_flags RandomXManager::getVMFlags() { return static_cast<randomx_flags>(flags); }

void RandomXManager::handleSeedHashChange(const std::string& newSeedHash)
{
    std::lock_guard<std::mutex> lock(seedHashMutex);
    if (newSeedHash != currentSeedHash) {
        {
            std::unique_lock<std::shared_mutex> vmLock(vmMutex);
            for (auto& [threadId, vm] : vms) { (void)threadId; if (vm) randomx_destroy_vm(vm); }
            vms.clear();
        }
        initialize(newSeedHash);
    }
}

bool RandomXManager::calculateHashForThread(int threadId, const std::vector<uint8_t>& input, uint64_t nonce)
{
    (void)nonce;
    randomx_vm* vm = nullptr;
    {
        std::shared_lock<std::shared_mutex> vmLock(vmMutex);
        auto it = vms.find(threadId);
        if (it == vms.end() || !it->second) return false;
        vm = it->second;
    }
    if (!initialized || input.empty() || input.size() > MAX_BLOB_SIZE) return false;

    alignas(64) uint8_t blob[MAX_BLOB_SIZE];
    alignas(64) uint8_t hash[RANDOMX_HASH_SIZE];
    memcpy(blob, input.data(), input.size());
    randomx_calculate_hash(vm, blob, input.size(), hash);

    static std::atomic<uint64_t> hashCounter{0};
    uint64_t count = hashCounter.fetch_add(1);
    if (config.debugMode && (count % 10000 == 0)) {
        std::stringstream ss;
        ss << "\n[RandomX] Hash #" << count << "\n  Input blob (first 50 bytes): ";
        for (size_t i = 0; i < 50 && i < input.size(); ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(input[i]) << " ";
        uint64_t hashHigh = 0;
        for (size_t i = 0; i < 8; ++i) hashHigh |= static_cast<uint64_t>(hash[24 + i]) << (i * 8);
        ss << "\n  Hash high: 0x" << std::hex << std::setw(16) << std::setfill('0') << hashHigh;
        {
            std::lock_guard<std::mutex> lock(targetMutex);
            ss << " | Target high: 0x" << std::hex << std::setw(16) << std::setfill('0') << expandedTarget.data[3];
        }
        Utils::threadSafePrint(ss.str(), true);
    }
    return checkTarget(hash);
}

double RandomXManager::getDifficulty()
{
    std::lock_guard<std::mutex> lock(targetMutex);
    return currentDifficulty;
}
