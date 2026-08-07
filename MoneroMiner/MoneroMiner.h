#pragma once

#include "Types.h"
#include "Constants.h"
#include "HashBuffers.h"
#include "MiningThreadData.h"
#include "Job.h"
#include "Config.h"
#include "MiningStats.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "picojson.h"

// ============================================================
// Jobs
// ============================================================

extern std::mutex jobMutex;
extern std::queue<Job> jobQueue;

extern std::condition_variable jobAvailable;
extern std::condition_variable jobQueueCV;

extern std::atomic<bool> shouldStop;
extern std::atomic<bool> newJobAvailable;
extern std::atomic<bool> showedInitMessage;

// ============================================================
// Job atual
// ============================================================

extern std::mutex currentJobMutex;

extern std::string currentBlobHex;
extern std::string currentTargetHex;
extern std::string currentJobId;
extern std::string currentSeedHash;

extern std::atomic<uint32_t> activeJobId;
extern std::atomic<uint32_t> notifiedJobId;

// ============================================================
// Hashes
// ============================================================

extern std::atomic<uint64_t> totalHashes;
extern std::atomic<uint32_t> debugHashCounter;

// ============================================================
// Sessão
// ============================================================

extern std::atomic<uint64_t> jsonRpcId;
extern std::string sessionId;

// ============================================================
// Workers
// ============================================================

extern std::atomic<bool> workersStarted;

extern std::vector<MiningThreadData*> threadData;
extern std::vector<std::thread> miningThreads;

// ============================================================
// Stats Web
// ============================================================

extern std::thread statsWebThread;
extern std::atomic<bool> statsThreadRunning;

// ============================================================
// Saída
// ============================================================

extern std::mutex consoleMutex;
extern std::mutex logfileMutex;
extern std::ofstream logFile;

extern bool debugMode;

// ============================================================
// Funções
// ============================================================

void signalHandler(int signum);

void miningThread(
    MiningThreadData* data
);

void processNewJob(
    const picojson::object& jobObj
);

void notifyNewJob();

bool getCurrentJob(
    Job& outJob
);

bool submitShare(
    const std::string& jobId,
    const std::string& nonce,
    const std::string& hash,
    const std::string& algo
);

void handleLoginResponse(
    const std::string& response
);

void handleShareResponse(
    const std::string& response,
    bool& accepted
);

std::string createSubmitPayload(
    const std::string& sessionId,
    const std::string& jobId,
    const std::string& nonceHex,
    const std::string& hashHex,
    const std::string& algo
);

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

bool loadConfig();

bool validateConfig();

bool parseCommandLine(
    int argc,
    char* argv[]
);

void printHelp();

void printConfig();

// ============================================================
// WASM
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
