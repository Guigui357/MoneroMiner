#pragma once

#include "Types.h"
#include "Constants.h"
#include "HashBuffers.h"
#include "MiningThreadData.h"
#include "Job.h"
#include "Config.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "picojson.h"

// ============================================================
// Plataforma
// ============================================================

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/websocket.h>

using MinerSocket = EMSCRIPTEN_WEBSOCKET_T;

#else

#ifdef _WIN32
#include <winsock2.h>
using MinerSocket = SOCKET;
#else
#include <sys/socket.h>
using MinerSocket = int;
#endif

#endif


// ============================================================
// Globais do minerador
// ============================================================

extern std::mutex jobMutex;

extern std::queue<Job> jobQueue;

extern std::condition_variable jobAvailable;
extern std::condition_variable jobQueueCV;

extern std::atomic<bool> shouldStop;

extern std::atomic<bool> newJobAvailable;

extern std::atomic<bool> showedInitMessage;

extern std::atomic<bool> workersStarted;


// ============================================================
// Estado do Job atual
// ============================================================

extern std::mutex currentJobMutex;

extern std::string currentBlobHex;
extern std::string currentTargetHex;
extern std::string currentJobId;
extern std::string currentSeedHash;

extern std::atomic<uint32_t> activeJobId;
extern std::atomic<uint32_t> notifiedJobId;


// ============================================================
// Estatísticas
// ============================================================

extern std::atomic<uint64_t> totalHashes;
extern std::atomic<uint64_t> acceptedShares;
extern std::atomic<uint64_t> rejectedShares;

extern std::atomic<uint32_t> debugHashCounter;


// ============================================================
// Sessão / JSON-RPC
// ============================================================

extern std::atomic<uint64_t> jsonRpcId;

extern std::string sessionId;


// ============================================================
// Threads de mineração
// ============================================================

extern std::vector<MiningThreadData*> threadData;

extern std::vector<std::thread> miningThreads;


// ============================================================
// Controle de estatísticas Web
// ============================================================

extern std::thread statsWebThread;

extern std::atomic<bool> statsThreadRunning;


// ============================================================
// Configuração
// ============================================================

extern Config config;


// ============================================================
// Estatísticas globais
// ============================================================

extern GlobalStats globalStats;


// ============================================================
// Mutexes de saída
// ============================================================

extern std::mutex consoleMutex;
extern std::mutex logfileMutex;

extern std::ofstream logFile;

extern bool debugMode;


// ============================================================
// Funções principais do minerador
// ============================================================

void signalHandler(int signum);

void miningThread(
    MiningThreadData* data
);


// ============================================================
// Workers
// ============================================================
//
// IMPORTANTE:
// Esta função deve ser chamada SOMENTE depois que o primeiro
// Job válido for recebido.
//
// No WASM, não chame isso diretamente durante o callback de
// criação do WebSocket antes do Job existir.
//

void startMiningWorkers();


// Opcional: parada explícita dos workers

void stopMiningWorkers();


// ============================================================
// Jobs
// ============================================================

void processNewJob(
    const picojson::object& jobObj
);


// Notifica todas as threads de mineração que existe um novo Job.

void notifyNewJob();


// Obtém o Job atual com segurança.

bool getCurrentJob(
    Job& outJob
);


// ============================================================
// Pool / comunicação
// ============================================================

bool submitShare(
    const std::string& jobId,
    const std::string& nonce,
    const std::string& hash,
    const std::string& algo
);


// ============================================================
// Respostas da pool
// ============================================================

void handleLoginResponse(
    const std::string& response
);

void handleShareResponse(
    const std::string& response,
    bool& accepted
);


// ============================================================
// JSON-RPC
// ============================================================

std::string createSubmitPayload(
    const std::string& sessionId,
    const std::string& jobId,
    const std::string& nonceHex,
    const std::string& hashHex,
    const std::string& algo
);


// ============================================================
// Stats
// ============================================================

void updateThreadStats(
    MiningThreadData* data,
    uint64_t hashCount,
    uint64_t totalHashCount,
    int elapsedSeconds,
    const std::string& jobId,
    uint32_t currentNonce
);

void globalStatsMonitor();

void webStatsMonitorLoop();


// ============================================================
// Configuração / CLI
// ============================================================

bool loadConfig();

bool validateConfig();

bool parseCommandLine(
    int argc,
    char* argv[]
);

void printHelp();

void printConfig();


// ============================================================
// WASM API
// ============================================================

#ifdef __EMSCRIPTEN__

extern "C" {

bool startMining(
    const char* customWallet,
    const char* customWorker
);

bool stopMining();

}

#endif
