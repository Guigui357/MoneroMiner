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


// ============================================================
// INITIALIZE CACHE
// ============================================================

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

    // ========================================================
    // Converter seed hexadecimal -> bytes
    // ========================================================

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

    // ========================================================
    // Detectar flags disponíveis
    // ========================================================

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

    // ========================================================
    // CONFIGURAÇÃO WASM
    //
    // IMPORTANTE:
    // initializeCache() NÃO cria VM.
    //
    // A VM é criada posteriormente por:
    // createVM(int threadId)
    // ========================================================

#ifdef __EMSCRIPTEN__

#ifdef RANDOMX_WASM_FAST

    // ========================================================
    // WASM FAST / FULL_MEM
    // ========================================================

    useLightMode = false;

    // Cache continua sem JIT no navegador.
    cacheAllocFlags = RANDOMX_FLAG_DEFAULT;

    // A VM e o dataset usam FULL_MEM.
    flags = RANDOMX_FLAG_FULL_MEM;

    Utils::threadSafePrint(
        "[WASM] RandomX FAST MODE",
        true
    );

    Utils::threadSafePrint(
        "[WASM] FULL_MEM ativado",
        true
    );

    Utils::threadSafePrint(
        "[WASM] LARGE_PAGES desativado",
        true
    );

    Utils::threadSafePrint(
        "[WASM] JIT desativado para compatibilidade",
        true
    );

#else

    // ========================================================
    // WASM LIGHT
    // ========================================================

    useLightMode = true;

    cacheAllocFlags = RANDOMX_FLAG_DEFAULT;
    flags = RANDOMX_FLAG_DEFAULT;

    Utils::threadSafePrint(
        "[WASM] RandomX LIGHT MODE",
        true
    );

    Utils::threadSafePrint(
        "[WASM] FULL_MEM desativado",
        true
    );

    Utils::threadSafePrint(
        "[WASM] LARGE_PAGES desativado",
        true
    );

    Utils::threadSafePrint(
        "[WASM] JIT desativado para compatibilidade",
        true
    );

#endif

#else

    // ========================================================
    // Liberar cache anterior
    // ========================================================

    if (cache != nullptr)
    {
        Utils::threadSafePrint(
            "[RandomX] Liberando cache anterior...",
            true
        );

        randomx_release_cache(cache);

        cache = nullptr;
    }

    // ========================================================
    // Alocar cache
    // ========================================================

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

    // ========================================================
    // Inicializar cache com seed
    // ========================================================

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

    currentSeedHash = seedHash;

    return true;
}

// ============================================================
// CREATE DATASET
// ============================================================
bool RandomXManager::createDataset()
{
#ifdef __EMSCRIPTEN__
#ifndef RANDOMX_WASM_FAST

    Utils::threadSafePrint(
        "[WASM] createDataset() ignorado: usando LIGHT MODE",
        true
    );

    return false;

#endif
#endif

    if (!cache) {
        Utils::threadSafePrint(
            "[RandomX] ERRO: cache não inicializado",
            true
        );
        return false;
    }

    // Release previous dataset if one exists.
    if (dataset) {
        Utils::threadSafePrint(
            "[RandomX] Liberando dataset anterior...",
            true
        );

        randomx_release_dataset(dataset);
        dataset = nullptr;
    }

    Utils::threadSafePrint(
        "[RandomX] Alocando dataset FULL_MEM...",
        true
    );

    dataset = randomx_alloc_dataset(flags);

    if (!dataset) {
        Utils::threadSafePrint(
            "[RandomX] Falha ao alocar dataset com flags atuais",
            true
        );

#ifdef __EMSCRIPTEN__
        Utils::threadSafePrint(
            "[WASM] Falha ao alocar dataset de ~2.08 GiB",
            true
        );
#else
        // Desktop fallback.
        flags = RANDOMX_FLAG_FULL_MEM;

        Utils::threadSafePrint(
            "[RandomX] Tentando novamente com RANDOMX_FLAG_FULL_MEM...",
            true
        );

        dataset = randomx_alloc_dataset(flags);

        if (!dataset) {
            Utils::threadSafePrint(
                "[RandomX] Falha ao alocar dataset FULL_MEM",
                true
            );
            return false;
        }
#endif
    }

    const uint64_t itemCount = randomx_dataset_item_count();

    Utils::threadSafePrint(
        "[RandomX] Dataset items: " +
        std::to_string(itemCount),
        true
    );

#ifdef __EMSCRIPTEN__

    Utils::threadSafePrint(
        "[WASM] Inicializando dataset RandomX FULL_MEM...",
        true
    );

    const unsigned int hardwareThreads =
        std::thread::hardware_concurrency();

    unsigned int numThreads =
        hardwareThreads > 1 ? hardwareThreads - 1 : 1;

    if (numThreads > 8) {
        numThreads = 8;
    }

    Utils::threadSafePrint(
        "[WASM] Threads para inicialização do dataset: " +
        std::to_string(numThreads),
        true
    );

#else

    unsigned int numThreads =
        std::thread::hardware_concurrency();

    if (numThreads == 0) {
        numThreads = 1;
    }

    if (numThreads > 1) {
        numThreads--;
    }

#endif

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    const uint64_t itemsPerThread =
        itemCount / numThreads;

    const uint64_t remainder =
        itemCount % numThreads;

    auto startTime = std::chrono::steady_clock::now();

    uint64_t startIndex = 0;

    for (unsigned int i = 0; i < numThreads; ++i) {

        const uint64_t count =
            itemsPerThread +
            (i < remainder ? 1 : 0);

        const uint64_t threadStart = startIndex;

        threads.emplace_back(
            [this, threadStart, count]() {

                randomx_init_dataset(
                    dataset,
                    cache,
                    threadStart,
                    count
                );
            }
        );

        startIndex += count;
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto endTime = std::chrono::steady_clock::now();

    const double elapsed =
        std::chrono::duration<double>(endTime - startTime).count();

    Utils::threadSafePrint(
        "[RandomX] Dataset inicializado em " +
        std::to_string(elapsed) +
        " segundos",
        true
    );

    if (!dataset) {
        Utils::threadSafePrint(
            "[RandomX] ERRO: dataset ficou nulo após inicialização",
            true
        );
        return false;
    }

#ifdef __EMSCRIPTEN__
    Utils::threadSafePrint(
        "[WASM] Dataset FULL_MEM pronto",
        true
    );
#else
    Utils::threadSafePrint(
        "[RandomX] Dataset FULL_MEM pronto",
        true
    );
#endif

    return true;
}


// ============================================================
// INITIALIZE RANDOMX
// ============================================================

bool RandomXManager::initialize(
    const std::string& seedHash
)
{
    std::lock_guard<std::mutex> lock(initMutex);

    Utils::threadSafePrint(
        "[WASM-DEBUG] >>> RandomXManager::initialize() ENTROU",
        true
    );

    Utils::threadSafePrint(
        "[WASM-DEBUG] seedHash = " + seedHash,
        true
    );

    if (seedHash.empty())
    {
        Utils::threadSafePrint(
            "[WASM-DEBUG] ERRO: seedHash vazio",
            true
        );

        return false;
    }

    if (seedHash.size() != 64)
    {
        Utils::threadSafePrint(
            "[WASM-DEBUG] ERRO: seedHash possui tamanho " +
            std::to_string(seedHash.size()) +
            ", esperado 64",
            true
        );

        return false;
    }

    // Se já está inicializado para a mesma seed,
    // não recria o cache.
    if (
        seedHash == currentSeedHash &&
        cache != nullptr &&
        initialized
    )
    {
        Utils::threadSafePrint(
            "[WASM-DEBUG] RandomX já inicializado para esta seed",
            true
        );

        return true;
    }

    Utils::threadSafePrint(
        "[WASM-DEBUG] Chamando initializeCache()...",
        true
    );

    if (!initializeCache(seedHash))
    {
        Utils::threadSafePrint(
            "[WASM-DEBUG] ERRO: initializeCache() falhou",
            true
        );

        initialized = false;
        return false;
    }

    Utils::threadSafePrint(
        "[WASM-DEBUG] initializeCache() OK",
        true
    );

#ifdef __EMSCRIPTEN__

#ifdef RANDOMX_WASM_FAST

    // ========================================================
    // WASM FAST
    // ========================================================

    useLightMode = false;
    flags = RANDOMX_FLAG_FULL_MEM;

    Utils::threadSafePrint(
        "[WASM] RANDOMX FAST MODE",
        true
    );

    Utils::threadSafePrint(
        "[WASM] FULL_MEM ativado",
        true
    );

    Utils::threadSafePrint(
        "[WASM] Criando dataset RandomX (~2.08 GiB)...",
        true
    );

    if (!createDataset())
    {
        Utils::threadSafePrint(
            "[WASM] ERRO CRÍTICO: dataset FULL_MEM não pôde ser criado",
            true
        );

        initialized = false;
        return false;
    }

    if (dataset == nullptr)
    {
        Utils::threadSafePrint(
            "[WASM] ERRO CRÍTICO: createDataset() retornou sem dataset",
            true
        );

        initialized = false;
        return false;
    }

    Utils::threadSafePrint(
        "[WASM] Dataset FULL_MEM pronto",
        true
    );

#else

    // ========================================================
    // WASM LIGHT
    // ========================================================

    useLightMode = true;
    flags = RANDOMX_FLAG_DEFAULT;

    Utils::threadSafePrint(
        "[WASM] RandomX LIGHT MODE",
        true
    );

    Utils::threadSafePrint(
        "[WASM] Dataset não será criado",
        true
    );

#endif

#else

    if (!useLightMode)
    {
        std::string datasetFileName =
            "randomx_dataset_" +
            seedHash.substr(0, 16) +
            ".bin";

        bool loadedDataset = false;

        if (std::filesystem::exists(datasetFileName))
        {
            size_t fileSize =
                std::filesystem::file_size(datasetFileName);

            unsigned long itemCount =
                randomx_dataset_item_count();

            size_t expectedMinSize =
                static_cast<size_t>(itemCount) *
                RANDOMX_DATASET_ITEM_SIZE;

            if (fileSize >= expectedMinSize)
            {
                Utils::threadSafePrint(
                    "Loading dataset from disk...",
                    true
                );

                loadedDataset =
                    loadDataset(datasetFileName);
            }
            else
            {
                std::filesystem::remove(datasetFileName);
            }
        }

        if (!loadedDataset)
        {
            Utils::threadSafePrint(
                "=== CREATING 2GB RANDOMX DATASET ===",
                true
            );

            if (!createDataset())
            {
                Utils::threadSafePrint(
                    "Dataset creation failed; switching to LIGHT MODE",
                    true
                );

                useLightMode = true;
                flags = cacheAllocFlags;
            }
            else
            {
                saveDataset(datasetFileName);
            }
        }
    }

#endif

    currentSeedHash = seedHash;
    initialized = true;

    Utils::threadSafePrint(
        "[WASM-DEBUG] initialized = true",
        true
    );

    Utils::threadSafePrint(
        "[WASM-DEBUG] cache = " +
        std::string(cache ? "VALID" : "NULL"),
        true
    );

    Utils::threadSafePrint(
        "[WASM-DEBUG] currentSeedHash = " +
        currentSeedHash,
        true
    );

    Utils::threadSafePrint(
        "=== RANDOMX READY ===",
        true
    );

    return true;
}


// ============================================================
// CREATE VM
// ============================================================

bool RandomXManager::createVM(int threadId)
{
    std::unique_lock<std::shared_mutex> lock(
        vmMutex
    );

    if (!initialized || cache == nullptr)
    {
        Utils::threadSafePrint(
            "[WASM] ERRO: RandomX não está inicializado "
            "ou cache == nullptr",
            true
        );

        return false;
    }

    // ========================================================
    // Já existe VM para esta thread?
    // ========================================================

    auto existing =
        vms.find(threadId);

    if (
        existing != vms.end() &&
        existing->second != nullptr
    )
    {
        Utils::threadSafePrint(
            "[RandomX] VM já existe para thread "
            +
            std::to_string(threadId),
            true
        );

        return true;
    }

#ifdef __EMSCRIPTEN__

#ifdef RANDOMX_WASM_FAST

    // ========================================================
    // WASM FAST / FULL_MEM
    // ========================================================

    if (dataset == nullptr)
    {
        Utils::threadSafePrint(
            "[WASM] ERRO: FULL_MEM requer dataset",
            true
        );

        return false;
    }

    randomx_flags wasmFlags =
        RANDOMX_FLAG_FULL_MEM;

    // JIT deliberadamente não utilizado no WASM.
    wasmFlags = static_cast<randomx_flags>(
        wasmFlags & ~RANDOMX_FLAG_JIT
    );

    Utils::threadSafePrint(
        "[WASM] Criando VM FAST para thread "
        +
        std::to_string(threadId),
        true
    );

    Utils::threadSafePrint(
        "[WASM] VM flags: 0x"
        +
        Utils::formatHex(
            static_cast<uint64_t>(wasmFlags),
            8
        ),
        true
    );

    Utils::threadSafePrint(
        "[WASM] Cache: "
        +
        std::string(
            cache != nullptr
            ?
            "OK"
            :
            "NULL"
        ),
        true
    );

    Utils::threadSafePrint(
        "[WASM] Dataset: "
        +
        std::string(
            dataset != nullptr
            ?
            "VALID"
            :
            "NULL"
        ),
        true
    );

    Utils::threadSafePrint(
        "[WASM] Chamando randomx_create_vm() FULL_MEM...",
        true
    );

    randomx_vm* vm =
        randomx_create_vm(
            wasmFlags,
            cache,
            dataset
        );

    if (vm == nullptr)
    {
        Utils::threadSafePrint(
            "[WASM] ERRO: randomx_create_vm() FULL_MEM "
            "retornou nullptr",
            true
        );

        return false;
    }

    vms[threadId] = vm;

    Utils::threadSafePrint(
        "[WASM] VM FAST/FULL_MEM criada com sucesso "
        "para thread "
        +
        std::to_string(threadId),
        true
    );

    return true;

#else

    // ========================================================
    // WASM LIGHT
    // ========================================================

    randomx_flags wasmFlags =
        RANDOMX_FLAG_DEFAULT;

    Utils::threadSafePrint(
        "[WASM] Criando VM LIGHT para thread "
        +
        std::to_string(threadId),
        true
    );

    Utils::threadSafePrint(
        "[WASM] VM flags: 0x"
        +
        Utils::formatHex(
            static_cast<uint64_t>(wasmFlags),
            8
        ),
        true
    );

    Utils::threadSafePrint(
        "[WASM] Cache: "
        +
        std::string(
            cache != nullptr
            ?
            "OK"
            :
            "NULL"
        ),
        true
    );

    Utils::threadSafePrint(
        "[WASM] Dataset: NONE",
        true
    );

    randomx_vm* vm =
        randomx_create_vm(
            wasmFlags,
            cache,
            nullptr
        );

    if (vm == nullptr)
    {
        Utils::threadSafePrint(
            "[WASM] ERRO: randomx_create_vm() "
            "retornou nullptr",
            true
        );

        return false;
    }

    vms[threadId] = vm;

    Utils::threadSafePrint(
        "[WASM] VM LIGHT criada com sucesso "
        "para thread "
        +
        std::to_string(threadId),
        true
    );

    return true;

#endif

#else

    // ========================================================
    // DESKTOP
    // ========================================================

    if (
        !useLightMode &&
        dataset == nullptr
    )
    {
        Utils::threadSafePrint(
            "Cannot create VM: dataset required for full mode",
            true
        );

        return false;
    }

    randomx_vm* vm =
        randomx_create_vm(
            static_cast<randomx_flags>(flags),
            cache,
            useLightMode
                ?
                nullptr
                :
                dataset
        );

    if (vm == nullptr)
    {
        Utils::threadSafePrint(
            "VM creation failed",
            true
        );

        return false;
    }

    vms[threadId] = vm;

    return true;

#endif
}


// ============================================================
// INITIALIZE VM
// ============================================================

bool RandomXManager::initializeVM(int threadId)
{
    Utils::threadSafePrint(
        "[WASM-DEBUG] >>> initializeVM(" +
        std::to_string(threadId) +
        ") ENTROU",
        true
    );

    Utils::threadSafePrint(
        "[WASM-DEBUG] initialized = " +
        std::string(initialized ? "true" : "false"),
        true
    );

    Utils::threadSafePrint(
        "[WASM-DEBUG] cache = " +
        std::string(cache ? "VALID" : "NULL"),
        true
    );

    if (!initialized)
    {
        Utils::threadSafePrint(
            "[WASM-DEBUG] ERRO: initialize() ainda não foi concluído",
            true
        );

        return false;
    }

    if (cache == nullptr)
    {
        Utils::threadSafePrint(
            "[WASM-DEBUG] ERRO: cache == nullptr",
            true
        );

        return false;
    }

    Utils::threadSafePrint(
        "[WASM-DEBUG] Chamando createVM()...",
        true
    );

    bool result = createVM(threadId);

    Utils::threadSafePrint(
        "[WASM-DEBUG] createVM() retornou " +
        std::string(result ? "TRUE" : "FALSE"),
        true
    );

    return result;
}


// ============================================================
// GET VM
// ============================================================

randomx_vm* RandomXManager::getVM(int threadId)
{
    std::shared_lock<std::shared_mutex> lock(
        vmMutex
    );

    auto it =
        vms.find(threadId);

    return
        (it != vms.end())
        ?
        it->second
        :
        nullptr;
}


// ============================================================
// LOAD DATASET
// ============================================================

bool RandomXManager::loadDataset(
    const std::string& filename
)
{
#ifdef __EMSCRIPTEN__

    Utils::threadSafePrint(
        "[WASM] loadDataset() não utilizado no Fast WASM",
        true
    );

    return false;

#else

    unsigned long itemCount =
        randomx_dataset_item_count();

    size_t actualDatasetSize =
        static_cast<size_t>(itemCount)
        *
        RANDOMX_DATASET_ITEM_SIZE;

    if (!dataset)
    {
        dataset =
            randomx_alloc_dataset(
                static_cast<randomx_flags>(
                    flags
                )
            );

        if (!dataset)
            return false;
    }

    std::ifstream file(
        filename,
        std::ios::binary
    );

    if (!file.is_open())
        return false;

    void* datasetMemory =
        randomx_get_dataset_memory(
            dataset
        );

    if (!datasetMemory)
    {
        file.close();
        return false;
    }

    file.read(
        reinterpret_cast<char*>(
            datasetMemory
        ),
        actualDatasetSize
    );

    bool success =
        file.good() ||
        file.eof();

    file.close();

    return success;

#endif
}


// ============================================================
// SAVE DATASET
// ============================================================

bool RandomXManager::saveDataset(
    const std::string& filename
)
{
#ifdef __EMSCRIPTEN__

    return false;

#else

    if (!dataset)
        return false;

    unsigned long itemCount =
        randomx_dataset_item_count();

    size_t actualDatasetSize =
        static_cast<size_t>(itemCount)
        *
        RANDOMX_DATASET_ITEM_SIZE;

    std::ofstream file(
        filename,
        std::ios::binary
    );

    if (!file.is_open())
        return false;

    void* datasetMemory =
        randomx_get_dataset_memory(
            dataset
        );

    if (!datasetMemory)
    {
        file.close();
        return false;
    }

    file.write(
        reinterpret_cast<const char*>(
            datasetMemory
        ),
        actualDatasetSize
    );

    file.close();

    return true;

#endif
}


// ============================================================
// CLEANUP VM
// ============================================================

void RandomXManager::cleanupVM(
    int threadId
)
{
    std::unique_lock<std::shared_mutex> lock(
        vmMutex
    );

    auto it =
        vms.find(threadId);

    if (
        it != vms.end() &&
        it->second
    )
    {
        randomx_destroy_vm(
            it->second
        );

        vms.erase(it);
    }
}


// ============================================================
// DESTROY VM
// ============================================================

void RandomXManager::destroyVM(
    randomx_vm* vm
)
{
    if (!vm)
        return;

    std::unique_lock<std::shared_mutex> lock(
        vmMutex
    );

    for (
        auto it = vms.begin();
        it != vms.end();
        ++it
    )
    {
        if (it->second == vm)
        {
            randomx_destroy_vm(vm);

            vms.erase(it);

            break;
        }
    }
}


// ============================================================
// CLEANUP
// ============================================================

void RandomXManager::cleanup()
{
    std::lock_guard<std::mutex> lock(
        initMutex
    );

    {
        std::unique_lock<std::shared_mutex> vmLock(
            vmMutex
        );

        for (auto& [threadId, vm] : vms)
        {
            (void)threadId;

            if (vm)
            {
                randomx_destroy_vm(vm);
            }
        }

        vms.clear();
    }

    if (cache)
    {
        randomx_release_cache(cache);
        cache = nullptr;
    }

    if (dataset)
    {
        randomx_release_dataset(dataset);
        dataset = nullptr;
    }

    initialized = false;

    currentSeedHash.clear();
}


// ============================================================
// SET TARGET / DIFFICULTY
// ============================================================

bool RandomXManager::setTargetAndDifficulty(
    const std::string& targetHex
)
{
    if (targetHex.length() != 8)
    {
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> lock(
            targetMutex
        );

        std::vector<uint8_t> targetBytes =
            Utils::hexToBytes(targetHex);

        if (targetBytes.size() < 4)
        {
            return false;
        }

        uint32_t compactTarget = 0;

        for (size_t i = 0; i < 4; i++)
        {
            compactTarget |=
                static_cast<uint32_t>(
                    targetBytes[i]
                )
                <<
                (i * 8);
        }

        if (compactTarget == 0)
        {
            compactTarget = 1;
        }

        currentDifficulty =
            static_cast<double>(
                0xFFFFFFFFULL
            )
            /
            static_cast<double>(
                compactTarget
            );

        uint64_t diff64 =
            static_cast<uint64_t>(
                currentDifficulty
            );

        if (diff64 == 0)
        {
            diff64 = 1;
        }

        expandedTarget.data[0] =
            0xFFFFFFFFFFFFFFFFULL /
            diff64;

        expandedTarget.data[1] = 0;
        expandedTarget.data[2] = 0;
        expandedTarget.data[3] = 0;

        if (config.debugMode)
        {
            std::stringstream ss;

            ss
                << "[TARGET] 0x"
                << std::hex
                << compactTarget
                << " -> Diff:"
                << std::dec
                << diff64
                << " -> Target[0]=0x"
                << std::hex
                << std::setw(16)
                << std::setfill('0')
                << expandedTarget.data[0];

            Utils::threadSafePrint(
                ss.str(),
                true
            );
        }

        return true;
    }
    catch (const std::exception& e)
    {
        Utils::threadSafePrint(
            "Error parsing target: "
            +
            std::string(e.what()),
            true
        );

        return false;
    }
}


// ============================================================
// CHECK TARGET
// ============================================================

bool RandomXManager::checkTarget(
    const uint8_t* hash
)
{
    if (!hash)
        return false;

    uint256_t hashValue;

    for (int wordIdx = 0;
         wordIdx < 4;
         wordIdx++)
    {
        uint64_t word = 0;

        int baseByteIdx =
            wordIdx * 8;

        for (int byteInWord = 0;
             byteInWord < 8;
             byteInWord++)
        {
            word |=
                static_cast<uint64_t>(
                    hash[
                        baseByteIdx +
                        byteInWord
                    ]
                )
                <<
                (byteInWord * 8);
        }

        hashValue.data[wordIdx] =
            word;
    }

    // Compare MSW -> LSW
    for (int i = 3;
         i >= 0;
         i--)
    {
        if (
            hashValue.data[i]
            <
            expandedTarget.data[i]
        )
        {
            std::lock_guard<std::mutex> lock(
                hashMutex
            );

            lastHash.assign(
                hash,
                hash + RANDOMX_HASH_SIZE
            );

            std::stringstream ss;

            ss
                << "\n*** VALID SHARE FOUND ***\n";

            ss
                << "Hash (LE):   ";

            for (int w = 0;
                 w < 4;
                 w++)
            {
                ss
                    << std::hex
                    << std::setw(16)
                    << std::setfill('0')
                    << hashValue.data[w];
            }

            ss
                << "\nTarget (LE): ";

            for (int w = 0;
                 w < 4;
                 w++)
            {
                ss
                    << std::hex
                    << std::setw(16)
                    << std::setfill('0')
                    << expandedTarget.data[w];
            }

            ss
                << "\nFull hash: "
                << Utils::bytesToHex(
                    hash,
                    32
                );

            Utils::threadSafePrint(
                ss.str(),
                true
            );

            return true;
        }

        if (
            hashValue.data[i]
            >
            expandedTarget.data[i]
        )
        {
            return false;
        }
    }

    return true;
}


// ============================================================
// GET LAST HASH
// ============================================================

std::vector<uint8_t>
RandomXManager::getLastHash()
{
    std::lock_guard<std::mutex> lock(
        hashMutex
    );

    return lastHash;
}


// ============================================================
// GET LAST HASH HEX
// ============================================================

std::string
RandomXManager::getLastHashHex()
{
    std::lock_guard<std::mutex> lock(
        hashMutex
    );

    std::stringstream ss;

    ss
        << std::hex
        << std::setfill('0');

    for (uint8_t byte : lastHash)
    {
        ss
            << std::setw(2)
            << static_cast<int>(byte);
    }

    return ss.str();
}


// ============================================================
// GET DATASET
// ============================================================

randomx_dataset*
RandomXManager::getDataset()
{
    return dataset;
}


// ============================================================
// GET CACHE
// ============================================================

randomx_cache*
RandomXManager::getCache()
{
    return cache;
}


// ============================================================
// GET VM FLAGS
// ============================================================

randomx_flags
RandomXManager::getVMFlags()
{
    return static_cast<randomx_flags>(
        flags
    );
}


// ============================================================
// HANDLE SEED HASH CHANGE
// ============================================================

void RandomXManager::handleSeedHashChange(
    const std::string& newSeedHash
)
{
    std::lock_guard<std::mutex> lock(
        seedHashMutex
    );

    if (newSeedHash != currentSeedHash)
    {
        {
            std::unique_lock<std::shared_mutex> vmLock(
                vmMutex
            );

            for (
                auto& [threadId, vm] : vms
            )
            {
                (void)threadId;

                if (vm)
                {
                    randomx_destroy_vm(vm);
                }
            }

            vms.clear();
        }

        initialize(newSeedHash);
    }
}


// ============================================================
// CALCULATE HASH
// ============================================================

bool RandomXManager::calculateHashForThread(
    int threadId,
    const std::vector<uint8_t>& input,
    uint64_t nonce
)
{
    // Nonce mantido apenas para compatibilidade
    // com a API atual.
    (void)nonce;

    randomx_vm* vm = nullptr;

    {
        std::shared_lock<std::shared_mutex> vmLock(
            vmMutex
        );

        auto it =
            vms.find(threadId);

        if (
            it == vms.end() ||
            !it->second
        )
        {
            return false;
        }

        vm = it->second;
    }

    if (
        !initialized ||
        input.empty() ||
        input.size() > MAX_BLOB_SIZE
    )
    {
        return false;
    }

    alignas(64)
    uint8_t blob[MAX_BLOB_SIZE];

    alignas(64)
    uint8_t hash[RANDOMX_HASH_SIZE];

    // O nonce já está no blob.
    memcpy(
        blob,
        input.data(),
        input.size()
    );

    randomx_calculate_hash(
        vm,
        blob,
        input.size(),
        hash
    );

    static std::atomic<uint64_t>
        hashCounter{0};

    uint64_t count =
        hashCounter.fetch_add(1);

    if (
        config.debugMode &&
        (count % 10000 == 0)
    )
    {
        std::stringstream ss;

        ss
            << "\n[RandomX] Hash #"
            << count;

        ss
            << "\n  Input blob "
               "(first 50 bytes): ";

        for (
            size_t i = 0;
            i < 50 &&
            i < input.size();
            i++
        )
        {
            ss
                << std::hex
                << std::setw(2)
                << std::setfill('0')
                << static_cast<int>(
                    input[i]
                )
                << " ";
        }

        ss
            << "\n  Hash LSW: 0x"
            << std::hex
            << std::setw(16)
            << std::setfill('0');

        uint64_t hashLSW = 0;

        for (int i = 0;
             i < 8;
             i++)
        {
            hashLSW |=
                static_cast<uint64_t>(
                    hash[i]
                )
                <<
                (i * 8);
        }

        ss << hashLSW;

        ss
            << " | Target LSW: 0x"
            << std::hex
            << std::setw(16)
            << std::setfill('0')
            << expandedTarget.data[0];

        Utils::threadSafePrint(
            ss.str(),
            true
        );
    }

    bool wouldBeValid =
        checkTarget(hash);

    if (wouldBeValid)
    {
        Utils::threadSafePrint(
            "\n!!! VALID SHARE DETECTED !!!",
            true
        );
    }

    return wouldBeValid;
}


// ============================================================
// GET DIFFICULTY
// ============================================================

double RandomXManager::getDifficulty()
{
    std::lock_guard<std::mutex> lock(
        targetMutex
    );

    return currentDifficulty;
}
