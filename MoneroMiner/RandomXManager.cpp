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

// Added missing headers for threads and C string functions
#include <thread>
#include <cstring>

// Undefine Windows min/max macros that interfere with std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

static constexpr size_t MAX_BLOB_SIZE = 128;

// Static member initialization
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
    Utils::threadSafePrint(
        "[RandomX] Inicializando cache...",
        true
    );

    if (seedHash.empty())
    {
        Utils::threadSafePrint(
            "[RandomX] ERRO: seed hash vazio",
            true
        );

        return false;
    }

    if (seedHash.size() != 64)
    {
        Utils::threadSafePrint(
            "[RandomX] ERRO: seed hash deve possuir 64 caracteres hex",
            true
        );

        return false;
    }

    // --------------------------------------------------
    // Converter seed hexadecimal -> bytes
    // --------------------------------------------------

    std::vector<uint8_t> seedBytes;

    try
    {
        seedBytes.reserve(32);

        for (size_t i = 0; i < seedHash.size(); i += 2)
        {
            unsigned int byteValue = 0;

            std::stringstream ss;

            ss << std::hex
               << seedHash.substr(i, 2);

            ss >> byteValue;

            seedBytes.push_back(
                static_cast<uint8_t>(byteValue)
            );
        }
    }
    catch (const std::exception& e)
    {
        Utils::threadSafePrint(
            std::string("[RandomX] ERRO convertendo seed: ")
            + e.what(),
            true
        );

        return false;
    }

    if (seedBytes.size() != 32)
    {
        Utils::threadSafePrint(
            "[RandomX] ERRO: seed possui tamanho inválido",
            true
        );

        return false;
    }

    // --------------------------------------------------
    // Detectar flags disponíveis
    // --------------------------------------------------

    int detectedFlags = randomx_get_flags();

    Utils::threadSafePrint(
        "[RandomX] CPU flags detectadas: 0x"
        +
        Utils::formatHex(
            static_cast<uint64_t>(detectedFlags),
            8
        ),
        true
    );

    // --------------------------------------------------
    // WASM
    // --------------------------------------------------

#ifdef __EMSCRIPTEN__

useLightMode = true;

// TESTE: sem JIT
flags = detectedFlags;

flags &= ~RANDOMX_FLAG_FULL_MEM;
flags &= ~RANDOMX_FLAG_LARGE_PAGES;

cacheAllocFlags = detectedFlags;

cacheAllocFlags &= ~RANDOMX_FLAG_FULL_MEM;
cacheAllocFlags &= ~RANDOMX_FLAG_LARGE_PAGES;

Utils::threadSafePrint(
    "[WASM] RandomX LIGHT MODE + JIT DESATIVADO",
    true
);

#else
    // --------------------------------------------------
    // Desktop
    // --------------------------------------------------

    useLightMode = false;

    cacheAllocFlags =
        detectedFlags &
        ~RANDOMX_FLAG_FULL_MEM;

    flags =
        detectedFlags |
        RANDOMX_FLAG_FULL_MEM;

    flags |= RANDOMX_FLAG_JIT;

    cacheAllocFlags |= RANDOMX_FLAG_JIT;

    if (Platform::hasHugePagesSupport())
    {
        flags |= RANDOMX_FLAG_LARGE_PAGES;
        cacheAllocFlags |= RANDOMX_FLAG_LARGE_PAGES;

        Utils::threadSafePrint(
            "[RandomX] Large pages habilitadas",
            true
        );
    }
    else
    {
        Utils::threadSafePrint(
            "[RandomX] Large pages indisponíveis",
            true
        );
    }

#endif

    // --------------------------------------------------
    // Liberar cache anterior
    // --------------------------------------------------

    if (cache != nullptr)
    {
        Utils::threadSafePrint(
            "[RandomX] Liberando cache anterior...",
            true
        );

        randomx_release_cache(cache);

        cache = nullptr;
    }

    // --------------------------------------------------
    // Alocar cache
    // --------------------------------------------------

    Utils::threadSafePrint(
        "[RandomX] Alocando RandomX cache...",
        true
    );

    cache =
        randomx_alloc_cache(
            static_cast<randomx_flags>(
                cacheAllocFlags
            )
        );

    if (cache == nullptr)
    {
        Utils::threadSafePrint(
            "[RandomX] ERRO: randomx_alloc_cache() falhou",
            true
        );

        return false;
    }

    Utils::threadSafePrint(
        "[RandomX] Cache alocado",
        true
    );

    // --------------------------------------------------
    // Inicializar cache com seed
    // --------------------------------------------------

    Utils::threadSafePrint(
        "[RandomX] Inicializando cache com seed...",
        true
    );

    randomx_init_cache(
        cache,
        seedBytes.data(),
        seedBytes.size()
    );

    Utils::threadSafePrint(
        "[RandomX] Cache RandomX inicializado",
        true
    );

    // --------------------------------------------------
    // Guardar seed atual
    // --------------------------------------------------

    currentSeedHash = seedHash;

    return true;
}

bool RandomXManager::createDataset() {
    if (!cache) {
        Utils::threadSafePrint("Cannot create dataset: no cache", true);
        return false;
    }

    if (dataset) {
        randomx_release_dataset(dataset);
        dataset = nullptr;
    }

    Utils::threadSafePrint("Allocating dataset with flags: 0x" + Utils::formatHex(static_cast<uint64_t>(flags), 8), true);
    
    dataset = randomx_alloc_dataset(static_cast<randomx_flags>(flags));
    if (!dataset) {
        Utils::threadSafePrint("Dataset allocation failed, trying FULL_MEM only", true);
        flags = RANDOMX_FLAG_FULL_MEM;
        dataset = randomx_alloc_dataset(RANDOMX_FLAG_FULL_MEM);
        if (!dataset) {
            Utils::threadSafePrint("Dataset allocation failed", true);
            return false;
        }
    }

    unsigned long itemCount = randomx_dataset_item_count();
    Utils::threadSafePrint("Initializing " + std::to_string(itemCount) + " dataset items...", true);

    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 1;
    // Reserve one logical CPU for system responsiveness
    if (numThreads > 1) numThreads = (std::max)(1u, numThreads - 1u);
    Utils::threadSafePrint("Using " + std::to_string(numThreads) + " threads for dataset initialization (leaving 1 for system)", true);

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    unsigned long itemsPerThread = itemCount / numThreads;

    for (unsigned int t = 0; t < numThreads; t++) {
        unsigned long startIndex = t * itemsPerThread;
        unsigned long count = (t == numThreads - 1) ? (itemCount - startIndex) : itemsPerThread;
        threads.emplace_back([startIndex, count]() {
            randomx_init_dataset(dataset, cache, startIndex, count);
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    double seconds = duration.count() / 1000.0;
    
    Utils::threadSafePrint("Dataset initialized in " + std::to_string(seconds) + " seconds", true);
    
    return true;
}

bool RandomXManager::initialize(const std::string& seedHash) {
    std::lock_guard<std::mutex> lock(initMutex);
    
    if (seedHash == currentSeedHash && cache != nullptr && initialized) {
        if (useLightMode || dataset != nullptr) {
            Utils::threadSafePrint("RandomX already initialized for seed hash", true);
            return true;
        }
    }

    Utils::threadSafePrint("=== INITIALIZING RANDOMX ===", true);
    Utils::threadSafePrint("Seed hash: " + seedHash, true);
    
    if (!initializeCache(seedHash)) {
        Utils::threadSafePrint("Failed to initialize RandomX cache", true);
        return false;
    }
    
    // Flags are already set in initializeCache with huge pages and JIT enabled if available
    
    if (!useLightMode) {
        std::string datasetFileName = "randomx_dataset_" + seedHash.substr(0, 16) + ".bin";
        bool loadedDataset = false;
        
        if (std::filesystem::exists(datasetFileName)) {
            size_t fileSize = std::filesystem::file_size(datasetFileName);
            
            unsigned long itemCount = randomx_dataset_item_count();
            size_t expectedMinSize = static_cast<size_t>(itemCount) * RANDOMX_DATASET_ITEM_SIZE;
            
            if (fileSize >= expectedMinSize) {
                Utils::threadSafePrint("Loading dataset from disk...", true);
                if (loadDataset(datasetFileName)) {
                    loadedDataset = true;
                }
            } else {
                std::filesystem::remove(datasetFileName);
            }
        }

        if (!loadedDataset) {
            Utils::threadSafePrint("=== CREATING 2GB RANDOMX DATASET ===", true);
            if (!createDataset()) {
                useLightMode = true;
                flags = cacheAllocFlags;
            } else {
                saveDataset(datasetFileName);
            }
        }
    }

    currentSeedHash = seedHash;
    initialized = true;
    
    // Print XMRig-style summary
    std::stringstream summary;
    summary << "RandomX: allocated ";
    
    // Calculate total memory (cache + dataset)
    int cacheSize = 256; // 256 MB cache
    int datasetSize = useLightMode ? 0 : 2080; // 2080 MB dataset if full mode
    int totalSize = cacheSize + datasetSize;
    
    summary << totalSize << " MB (" << datasetSize << "+" << cacheSize << ")";
    
    // Huge pages percentage
    if (flags & RANDOMX_FLAG_LARGE_PAGES) {
        summary << " huge pages 100%";
    } else {
        summary << " huge pages 0%";
    }
    
    // Show flags
    if (flags & RANDOMX_FLAG_JIT) {
        summary << " +JIT";
    }
    if (flags & RANDOMX_FLAG_HARD_AES) {
        summary << " +AES";
    }
    if (flags & RANDOMX_FLAG_FULL_MEM) {
        summary << " +FULL";
    }
    
    Utils::threadSafePrint(summary.str(), true);
    
    if (config.debugMode) {
        Utils::threadSafePrint("=== RANDOMX READY ===", true);
        Utils::threadSafePrint("Flags: 0x" + Utils::formatHex(static_cast<uint64_t>(flags), 8), true);
    }
    
    return true;
}

bool RandomXManager::createVM(int threadId) {
    std::unique_lock<std::shared_mutex> lock(vmMutex);
    
    if (!initialized || !cache) {
        Utils::threadSafePrint("Cannot create VM: RandomX not initialized", true);
        return false;
    }
    
    if (!useLightMode && !dataset) {
        Utils::threadSafePrint("Cannot create VM: dataset required for full mode", true);
        return false;
    }

    auto it = vms.find(threadId);
    if (it != vms.end() && it->second != nullptr) {
        return true;
    }

    // Only log in debug mode
    if (config.debugMode) {
        Utils::threadSafePrint("Creating VM for thread " + std::to_string(threadId), true);
    }
    
    randomx_vm* vm = randomx_create_vm(
        static_cast<randomx_flags>(flags), 
        cache, 
        useLightMode ? nullptr : dataset
    );
    
    if (!vm) {
        Utils::threadSafePrint("VM creation failed, trying fallback...", true);
        int fallbackFlags = cacheAllocFlags & ~RANDOMX_FLAG_FULL_MEM;
        vm = randomx_create_vm(static_cast<randomx_flags>(fallbackFlags), cache, nullptr);
        if (!vm) {
            Utils::threadSafePrint("VM creation failed completely", true);
            return false;
        }
    }

    vms[threadId] = vm;
    if (config.debugMode) {
        Utils::threadSafePrint("VM created successfully for thread " + std::to_string(threadId), true);
    }
    return true;
}

bool RandomXManager::initializeVM(int threadId) {
    if (!initialized) return false;
    return createVM(threadId);
}

randomx_vm* RandomXManager::getVM(int threadId) {
    std::shared_lock<std::shared_mutex> lock(vmMutex);
    auto it = vms.find(threadId);
    return (it != vms.end()) ? it->second : nullptr;
}

bool RandomXManager::loadDataset(const std::string& filename) {
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
    file.close();
    return true;
}

bool RandomXManager::saveDataset(const std::string& filename) {
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
}

void RandomXManager::cleanupVM(int threadId) {
    std::unique_lock<std::shared_mutex> lock(vmMutex);
    auto it = vms.find(threadId);
    if (it != vms.end() && it->second) {
        randomx_destroy_vm(it->second);
        vms.erase(it);
    }
}

void RandomXManager::destroyVM(randomx_vm* vm) {
    if (!vm) return;
    std::unique_lock<std::shared_mutex> lock(vmMutex);
    for (auto it = vms.begin(); it != vms.end(); ++it) {
        if (it->second == vm) {
            randomx_destroy_vm(vm);
            vms.erase(it);
            break;
        }
    }
}

void RandomXManager::cleanup() {
    std::lock_guard<std::mutex> lock(initMutex);
    {
        std::unique_lock<std::shared_mutex> vmLock(vmMutex);
        for (auto& [threadId, vm] : vms) {
            if (vm) randomx_destroy_vm(vm);
        }
        vms.clear();
    }
    if (cache) { randomx_release_cache(cache); cache = nullptr; }
    if (dataset) { randomx_release_dataset(dataset); dataset = nullptr; }
    initialized = false;
    currentSeedHash.clear();
}

bool RandomXManager::setTargetAndDifficulty(const std::string& targetHex) {
    if (targetHex.length() != 8) {
        return false;
    }
    
    try {
        std::lock_guard<std::mutex> lock(targetMutex);
        
        // Parse 4-byte compact target
        std::vector<uint8_t> targetBytes = Utils::hexToBytes(targetHex);
        uint32_t compactTarget = 0;
        for (size_t i = 0; i < 4; i++) {
            compactTarget |= static_cast<uint32_t>(targetBytes[i]) << (i * 8);
        }
        
        if (compactTarget == 0) compactTarget = 1;
        
        // Calculate difficulty
        currentDifficulty = static_cast<double>(0xFFFFFFFFULL) / static_cast<double>(compactTarget);
        
        // Calculate 256-bit target
        uint64_t diff64 = static_cast<uint64_t>(currentDifficulty);
        
        expandedTarget.data[0] = 0xFFFFFFFFFFFFFFFFULL / diff64;
        expandedTarget.data[1] = 0;
        expandedTarget.data[2] = 0;
        expandedTarget.data[3] = 0;
        
        if (config.debugMode) {
            std::stringstream ss;
            ss << "[TARGET] 0x" << std::hex << compactTarget 
               << " -> Diff:" << std::dec << diff64
               << " -> Target[0]=0x" << std::hex << std::setw(16) << std::setfill('0') 
               << expandedTarget.data[0];
            Utils::threadSafePrint(ss.str(), true);
        }
        
        return true;
    }
    catch (const std::exception& e) {
        Utils::threadSafePrint("Error parsing target: " + std::string(e.what()), true);
        return false;
    }
}

bool RandomXManager::checkTarget(const uint8_t* hash) {
    if (!hash) return false;
    
    // Convert hash bytes to uint256_t (little-endian)
    uint256_t hashValue;
    for (int wordIdx = 0; wordIdx < 4; wordIdx++) {
        uint64_t word = 0;
        int baseByteIdx = wordIdx * 8;
        for (int byteInWord = 0; byteInWord < 8; byteInWord++) {
            word |= static_cast<uint64_t>(hash[baseByteIdx + byteInWord]) << (byteInWord * 8);
        }
        hashValue.data[wordIdx] = word;
    }
    
    // Compare 256-bit values (MSW to LSW)
    for (int i = 3; i >= 0; i--) {
        if (hashValue.data[i] < expandedTarget.data[i]) {
            // Valid share found!
            std::lock_guard<std::mutex> lock(hashMutex);
            lastHash.assign(hash, hash + RANDOMX_HASH_SIZE);
            
            std::stringstream ss;
            ss << "\n*** VALID SHARE FOUND ***\n";
            ss << "Hash (LE):   ";
            for (int w = 0; w < 4; w++) {
                ss << std::hex << std::setw(16) << std::setfill('0') << hashValue.data[w];
            }
            ss << "\nTarget (LE): ";
            for (int w = 0; w < 4; w++) {
                ss << std::hex << std::setw(16) << std::setfill('0') << expandedTarget.data[w];
            }
            ss << "\nFull hash: " << Utils::bytesToHex(hash, 32);
            Utils::threadSafePrint(ss.str(), true);
            
            return true;
        }
        if (hashValue.data[i] > expandedTarget.data[i]) {
            return false;
        }
        // Equal, continue to next word
    }
    
    // All words equal - valid (hash == target)
    return true;
}

std::vector<uint8_t> RandomXManager::getLastHash() {
    std::lock_guard<std::mutex> lock(hashMutex);
    return lastHash;
}

std::string RandomXManager::getLastHashHex() {
    std::lock_guard<std::mutex> lock(hashMutex);
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : lastHash) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

randomx_dataset* RandomXManager::getDataset() {
    return dataset;
}

randomx_cache* RandomXManager::getCache() {
    return cache;
}

randomx_flags RandomXManager::getVMFlags() {
    return static_cast<randomx_flags>(flags);
}

void RandomXManager::handleSeedHashChange(const std::string& newSeedHash) {
    std::lock_guard<std::mutex> lock(seedHashMutex);
    if (newSeedHash != currentSeedHash) {
        {
            std::unique_lock<std::shared_mutex> vmLock(vmMutex);
            for (auto& [threadId, vm] : vms) {
                if (vm) randomx_destroy_vm(vm);
            }
            vms.clear();
        }
        initialize(newSeedHash);
    }
}

bool RandomXManager::calculateHashForThread(int threadId, const std::vector<uint8_t>& input, uint64_t nonce) {
    // Note: nonce parameter kept for API compatibility but not used
    // The nonce is already embedded in the input blob by the calling code
    (void)nonce; // Suppress unused parameter warning
    
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
    
    // CRITICAL FIX: DON'T insert nonce - it's already in the input blob!
    // The calling code (MoneroMiner.cpp) already wrote the nonce to the blob
    memcpy(blob, input.data(), input.size());
    
    // Calculate hash directly
    randomx_calculate_hash(vm, blob, input.size(), hash);
    
    // Debug logging (only every 10000th hash)
    static std::atomic<uint64_t> hashCounter{0};
    uint64_t count = hashCounter.fetch_add(1);
    
    if (config.debugMode && (count % 10000 == 0)) {
        std::stringstream ss;
        ss << "\n[RandomX] Hash #" << count;
        ss << "\n  Input blob (first 50 bytes): ";
        for (size_t i = 0; i < 50 && i < input.size(); i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(input[i]) << " ";
        }
        ss << "\n  Hash LSW: 0x" << std::hex << std::setw(16) << std::setfill('0');
        uint64_t hashLSW = 0;
        for (int i = 0; i < 8; i++) {
            hashLSW |= static_cast<uint64_t>(hash[i]) << (i * 8);
        }
        ss << hashLSW;
        ss << " | Target LSW: 0x" << std::hex << std::setw(16) << std::setfill('0') << expandedTarget.data[0];
        Utils::threadSafePrint(ss.str(), true);
    }
    
    bool wouldBeValid = checkTarget(hash);
    
    if (wouldBeValid) {
        Utils::threadSafePrint("\n!!! VALID SHARE DETECTED !!!", true);
    }
    
    return wouldBeValid;
}

double RandomXManager::getDifficulty() {
    std::lock_guard<std::mutex> lock(targetMutex);
    return currentDifficulty;
}
