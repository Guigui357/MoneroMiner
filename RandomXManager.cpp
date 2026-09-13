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
    Utils::threadSafePrint(
        "[RandomX] CPU flags detectadas: 0x" +
        Utils::formatHex(static_cast<uint64_t>(detectedFlags), 8), true);

#ifdef __EMSCRIPTEN__
    useLightMode = true;
    cacheAllocFlags = RANDOMX_FLAG_DEFAULT;
    flags = RANDOMX_FLAG_DEFAULT;
    Utils::threadSafePrint("[WASM] RandomX LIGHT MODE", true);
    Utils::threadSafePrint("[WASM] FULL_MEM desativado", true);
    Utils::threadSafePrint("[WASM] LARGE_PAGES desativado", true);
    Utils::threadSafePrint("[WASM] WasmJit experimental habilitado; fallback do interpretador permanece ativo", true);
#else
    useLightMode = false;
    cacheAllocFlags = detectedFlags & ~RANDOMX_FLAG_FULL_MEM;
    flags = detectedFlags | RANDOMX_FLAG_FULL_MEM | RANDOMX_FLAG_JIT;
    cacheAllocFlags |= RANDOMX_FLAG_JIT;
    if (Platform::hasHugePagesSupport()) {
        flags |= RANDOMX_FLAG_LARGE_PAGES;
        cacheAllocFlags |= RANDOMX_FLAG_LARGE_PAGES;
    }
#endif

    if (cache != nullptr) {
        randomx_release_cache(cache);
        cache = nullptr;
    }

    Utils::threadSafePrint("[RandomX] Alocando RandomX cache...", true);
    cache = randomx_alloc_cache(static_cast<randomx_flags>(cacheAllocFlags));
    if (cache == nullptr) {
        Utils::threadSafePrint("[RandomX] ERRO: randomx_alloc_cache() falhou", true);
        return false;
    }

    Utils::threadSafePrint("[RandomX] Cache alocado", true);
    Utils::threadSafePrint("[RandomX] Inicializando cache com seed...", true);
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
    if (!dataset) return false;
    unsigned long itemCount = randomx_dataset_item_count();
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (!numThreads) numThreads = 1;
    if (numThreads > 1) numThreads = (std::max)(1u, numThreads - 1u);
    std::vector<std::thread> threads;
    unsigned long itemsPerThread = itemCount / numThreads;
    for (unsigned int t = 0; t < numThreads; ++t) {
        unsigned long startIndex = t * itemsPerThread;
        unsigned long count = (t == numThreads - 1) ? itemCount - startIndex : itemsPerThread;
        threads.emplace_back([startIndex, count]() {
            randomx_init_dataset(dataset, cache, startIndex, count);
        });
    }
    for (auto& thread : threads) thread.join();
    return true;
#endif
}

bool RandomXManager::initialize(const std::string& seedHash)
{
    std::lock_guard<std::mutex> lock(initMutex);
    Utils::threadSafePrint("[WASM-DEBUG] >>> RandomXManager::initialize() ENTROU", true);
    Utils::threadSafePrint("[WASM-DEBUG] seedHash = " + seedHash, true);
    if (seedHash.empty() || seedHash.size() != 64) return false;
    if (seedHash == currentSeedHash && cache != nullptr && initialized) {
        Utils::threadSafePrint("[WASM-DEBUG] RandomX já inicializado para esta seed", true);
        return true;
    }
    if (!initializeCache(seedHash)) {
        initialized = false;
        return false;
    }
#ifdef __EMSCRIPTEN__
    useLightMode = true;
    flags = RANDOMX_FLAG_DEFAULT;
    Utils::threadSafePrint("[WASM] RandomX LIGHT MODE", true);
    Utils::threadSafePrint("[WASM] Dataset não será criado", true);
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
            if (!createDataset()) {
                useLightMode = true;
                flags = cacheAllocFlags;
            } else {
                saveDataset(datasetFileName);
            }
        }
    }
#endif
    currentSeedHash = seedHash;
    initialized = true;
    Utils::threadSafePrint("[WASM-DEBUG] initialized = true", true);
    Utils::threadSafePrint("[WASM-DEBUG] cache = " + std::string(cache ? "VALID" : "NULL"), true);
    Utils::threadSafePrint("[WASM-DEBUG] currentSeedHash = " + currentSeedHash, true);
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
    randomx_flags wasmFlags = RANDOMX_FLAG_DEFAULT;
    Utils::threadSafePrint("[WASM] Criando VM LIGHT para thread " + std::to_string(threadId), true);
    Utils::threadSafePrint("[WASM] WasmJit será tentado pelo InterpretedVm; VM nativa RandomX JIT continua desligada", true);
    randomx_vm* vm = randomx_create_vm(wasmFlags, cache, nullptr);
    if (!vm) {
        Utils::threadSafePrint("[WASM] ERRO: randomx_create_vm() retornou nullptr", true);
        return false;
    }
    vms[threadId] = vm;
    Utils::threadSafePrint("[WASM] VM LIGHT criada com sucesso para thread " + std::to_string(threadId), true);
    return true;
#else
    if (!useLightMode && dataset == nullptr) return false;
    randomx_vm* vm = randomx_create_vm(static_cast<randomx_flags>(flags), cache, useLightMode ? nullptr : dataset);
    if (!vm) return false;
    vms[threadId] = vm;
    return true;
#endif
}

bool RandomXManager::initializeVM(int threadId)
{
    Utils::threadSafePrint("[WASM-DEBUG] >>> initializeVM(" + std::to_string(threadId) + ") ENTROU", true);
    if (!initialized || cache == nullptr) return false;
    return createVM(threadId);
}
