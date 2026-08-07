#pragma once

#include "Platform.h"
#include "picojson.h"
#include "Job.h"

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/websocket.h>
#endif


namespace PoolClient {


// ===============================
// Socket abstraction
// ===============================

#ifdef __EMSCRIPTEN__

    // Browser WebSocket
    extern EMSCRIPTEN_WEBSOCKET_T poolSocket;

#else

    // Native TCP socket
    extern socket_t poolSocket;

#endif



// ===============================
// State
// ===============================

extern std::mutex jobMutex;
extern std::mutex socketMutex;
extern std::mutex submitMutex;

extern std::queue<Job> jobQueue;

extern std::condition_variable jobAvailable;
extern std::condition_variable jobQueueCondition;

extern std::atomic<bool> shouldStop;

extern std::string currentSeedHash;
extern std::string sessionId;
extern std::string currentTargetHex;
extern std::string poolId;



// ===============================
// Initialization
// ===============================

bool initialize();

bool connect();

bool reconnect();

void cleanup();



// ===============================
// Authentication
// ===============================

bool login(
    const std::string& wallet,
    const std::string& password,
    const std::string& worker,
    const std::string& userAgent
);



// ===============================
// Job system
// ===============================

void jobListener();

void processNewJob(
    const picojson::object& jobObj
);

void distributeJob(
    const Job& job
);

void handleSeedHashChange(
    const std::string& newSeedHash
);



// ===============================
// Share submission
// ===============================

bool submitShare(
    const std::string& jobId,
    const std::string& nonceHex,
    const std::string& hashHex,
    const std::string& algo
);



// ===============================
// Network helpers
// ===============================

#ifdef __EMSCRIPTEN__

// WebSocket send
bool sendData(
    const std::string& data
);


// WebSocket callbacks
EM_BOOL on_ws_open(
    int eventType,
    const EmscriptenWebSocketOpenEvent* websocketEvent,
    void* userData
);


EM_BOOL on_ws_message(
    int eventType,
    const EmscriptenWebSocketMessageEvent* websocketEvent,
    void* userData
);


EM_BOOL on_ws_close(
    int eventType,
    const EmscriptenWebSocketCloseEvent* websocketEvent,
    void* userData
);


EM_BOOL on_ws_error(
    int eventType,
    const EmscriptenWebSocketErrorEvent* websocketEvent,
    void* userData
);


#else

// TCP send/receive
std::string sendAndReceive(
    const std::string& payload
);


std::string receiveData(
    socket_t sock
);


std::string sendData(
    const std::string& data
);

#endif



// ===============================
// Keepalive
// ===============================

void sendKeepalive();


}
