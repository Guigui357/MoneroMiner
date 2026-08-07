#include "Config.h"
#include "PoolClient.h"
#include "RandomXManager.h"
#include "MiningStats.h"
#include "MiningThreadData.h"
#include "Job.h"
#include "Globals.h"
#include "Platform.h"

#include <sstream>


namespace PoolClient {


// ==================================================
// Globals
// ==================================================

#ifdef __EMSCRIPTEN__

EMSCRIPTEN_WEBSOCKET_T poolSocket = 0;

#else

socket_t poolSocket = INVALID_SOCKET;

#endif


std::mutex jobMutex;
std::mutex socketMutex;
std::mutex submitMutex;

std::queue<Job> jobQueue;

std::condition_variable jobAvailable;
std::condition_variable jobQueueCondition;

std::atomic<bool> shouldStop(false);

std::string currentSeedHash;
std::string sessionId;
std::string currentTargetHex;
std::string poolId;


// ==================================================
// Initialize
// ==================================================

bool initialize()
{
    shouldStop = false;

    Utils::threadSafePrint(
        "[WASM] PoolClient inicializado",
        true
    );

    return true;
}



// ==================================================
// WebSocket callbacks
// ==================================================

#ifdef __EMSCRIPTEN__


EM_BOOL on_ws_open(
    int eventType,
    const EmscriptenWebSocketOpenEvent* event,
    void* userData
)
{
    (void)eventType;
    (void)event;
    (void)userData;


    Utils::threadSafePrint(
        "[WASM] WebSocket conectado",
        true
    );


    bool ok = login(
        config.walletAddress,
        config.password,
        config.workerName,
        config.userAgent
    );


    Utils::threadSafePrint(
        ok ?
        "[WASM] Login enviado":
        "[WASM] Falha enviando login",
        true
    );


    return EM_TRUE;
}



EM_BOOL on_ws_message(
    int eventType,
    const EmscriptenWebSocketMessageEvent* event,
    void* userData
)
{
    (void)eventType;
    (void)userData;


    std::string msg(
        (char*)event->data,
        event->numBytes
    );


    Utils::threadSafePrint(
        "[WASM] RX: " + msg,
        true
    );


    picojson::value json;

    std::string err =
        picojson::parse(
            json,
            msg
        );


    if(!err.empty())
    {
        Utils::threadSafePrint(
            "[WASM] JSON invalido",
            true
        );
        return EM_TRUE;
    }



    if(!json.is<picojson::object>())
        return EM_TRUE;


    auto obj =
        json.get<picojson::object>();



    // Job vindo do proxy
    if(
        obj.find("identifier") != obj.end()
        &&
        obj["identifier"].get<std::string>()
            == "job"
    )
    {

        processNewJob(obj);


        return EM_TRUE;
    }



    // Login OK
    if(
        obj.find("status") != obj.end()
    )
    {
        Utils::threadSafePrint(
            "[WASM] Status: "
            +
            obj["status"].get<std::string>(),
            true
        );
    }



    return EM_TRUE;
}



EM_BOOL on_ws_close(
    int eventType,
    const EmscriptenWebSocketCloseEvent* event,
    void* userData
)
{
    (void)eventType;
    (void)event;
    (void)userData;


    Utils::threadSafePrint(
        "[WASM] WebSocket fechado",
        true
    );


    poolSocket = 0;

    return EM_TRUE;
}



EM_BOOL on_ws_error(
    int eventType,
    const EmscriptenWebSocketErrorEvent* event,
    void* userData
)
{
    (void)eventType;
    (void)event;
    (void)userData;


    Utils::threadSafePrint(
        "[WASM] Erro WebSocket",
        true
    );


    return EM_TRUE;
}


#endif



// ==================================================
// Connect
// ==================================================

bool connect()
{

#ifdef __EMSCRIPTEN__


    EmscriptenWebSocketCreateAttributes attr;

    emscripten_websocket_init_create_attributes(
        &attr
    );


    attr.url =
        "wss://proxy-xmr.onrender.com";


    attr.protocols = nullptr;

    attr.createOnMainThread = EM_TRUE;



    poolSocket =
        emscripten_websocket_new(
            &attr
        );


    if(poolSocket <= 0)
    {
        Utils::threadSafePrint(
            "[WASM] Falha criando WebSocket",
            true
        );

        return false;
    }



    emscripten_websocket_set_onopen_callback(
        poolSocket,
        nullptr,
        on_ws_open
    );


    emscripten_websocket_set_onmessage_callback(
        poolSocket,
        nullptr,
        on_ws_message
    );


    emscripten_websocket_set_onclose_callback(
        poolSocket,
        nullptr,
        on_ws_close
    );


    emscripten_websocket_set_onerror_callback(
        poolSocket,
        nullptr,
        on_ws_error
    );



    Utils::threadSafePrint(
        "[WASM] WebSocket criado",
        true
    );


    return true;


#else

    return false;

#endif

}



// ==================================================
// Login
// ==================================================

bool login(
    const std::string& wallet,
    const std::string& password,
    const std::string& worker,
    const std::string& userAgent
)
{

    picojson::object params;


    params["login"] =
        picojson::value(wallet);


    params["pass"] =
        picojson::value(password);


    params["agent"] =
        picojson::value(userAgent);



    picojson::object root;


    root["id"] =
        picojson::value(1.0);


    root["method"] =
        picojson::value("login");


    root["params"] =
        picojson::value(params);



    std::string json =
        picojson::value(root).serialize();



#ifdef __EMSCRIPTEN__


    int result =
        emscripten_websocket_send_utf8_text(
            poolSocket,
            json.c_str()
        );


    if(result != EMSCRIPTEN_RESULT_SUCCESS)
    {
        return false;
    }


    Utils::threadSafePrint(
        "[WASM] LOGIN -> "
        + json,
        true
    );


    return true;


#else

    return false;

#endif

}



// ==================================================
// Cleanup
// ==================================================

void cleanup()
{

#ifdef __EMSCRIPTEN__

    if(poolSocket)
    {
        emscripten_websocket_close(
            poolSocket,
            1000,
            "shutdown"
        );

        poolSocket = 0;
    }

#endif


    while(!jobQueue.empty())
        jobQueue.pop();

}



// ==================================================
// Job placeholder
// ==================================================

void processNewJob(
    const picojson::object& jobObj
)
{
    (void)jobObj

}



}
