#include "PoolClient.h"

#include "Utils.h"
#include "Config.h"
#include "Globals.h"
#include "Job.h"
#include "Difficulty.h"

#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <chrono>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#include <emscripten/websocket.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif


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
        "[WASM] *** ONOPEN DISPAROU ***",
        true
    );

    Utils::threadSafePrint(
        "[WASM] Enviando LOGIN...",
        true
    );

    bool ok = login(
        config.walletAddress,
        config.password,
        config.workerName,
        config.userAgent
    );

    Utils::threadSafePrint(
        ok
            ? "[WASM] LOGIN ENVIADO"
            : "[WASM] FALHA AO ENVIAR LOGIN",
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
    (void)userData;

    Utils::threadSafePrint(
        "[WASM] *** WEBSOCKET FECHOU ***",
        true
    );

    if (event)
    {
        Utils::threadSafePrint(
            "[WASM] Close code: " +
            std::to_string(event->code),
            true
        );

        Utils::threadSafePrint(
            "[WASM] Close reason: " +
            std::string(event->reason),
            true
        );

        Utils::threadSafePrint(
            event->wasClean
                ? "[WASM] Fechamento limpo"
                : "[WASM] Fechamento NAO LIMPO",
            true
        );
    }

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

bool submitShare(
    const std::string& jobId,
    const std::string& nonceHex,
    const std::string& hashHex,
    const std::string& algo
) {
    picojson::object obj;

    obj["id"] = picojson::value(jobId);
    obj["nonce"] = picojson::value(nonceHex);
    obj["hash"] = picojson::value(hashHex);
    obj["algo"] = picojson::value(algo);

    picojson::object root;
    root["method"] = picojson::value("submit");
    root["params"] = picojson::value(obj);

    std::string payload = picojson::value(root).serialize();

    std::lock_guard<std::mutex> lock(submitMutex);

    return sendData(payload);
}

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

    attr.createOnMainThread = EM_FALSE;



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

bool sendData(const std::string& data)
{
    std::lock_guard<std::mutex> lock(socketMutex);

#ifdef __EMSCRIPTEN__

    if (poolSocket == 0) {
        Utils::threadSafePrint("[WS] Socket inválido", true);
        return false;
    }

    int result = emscripten_websocket_send_utf8_text(
        poolSocket,
        data.c_str()
    );

    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        Utils::threadSafePrint("[WS] Falha ao enviar", true);
        return false;
    }

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

void processNewJob(
    const picojson::object& jobObj
)
{
    try
    {
        std::string blob;
        std::string jobId;
        std::string target;
        std::string seed;
        uint64_t height = 0;


        if(jobObj.find("blob") != jobObj.end())
        {
            blob =
                jobObj.at("blob")
                .get<std::string>();
        }


        if(jobObj.find("job_id") != jobObj.end())
        {
            jobId =
                jobObj.at("job_id")
                .get<std::string>();
        }


        if(jobObj.find("target") != jobObj.end())
        {
            target =
                jobObj.at("target")
                .get<std::string>();
        }


        if(jobObj.find("seed_hash") != jobObj.end())
        {
            seed =
                jobObj.at("seed_hash")
                .get<std::string>();
        }


        if(jobObj.find("height") != jobObj.end())
        {
            height =
                static_cast<uint64_t>(
                    jobObj.at("height")
                    .get<double>()
                );
        }



        if(blob.empty() || jobId.empty())
        {
            Utils::threadSafePrint(
                "[WASM] JOB inválido",
                true
            );

            return;
        }



        Job job(
            blob,
            jobId,
            target,
            height,
            seed
        );



        {
            std::lock_guard<std::mutex> lock(
                jobMutex
            );


            while(!jobQueue.empty())
                jobQueue.pop();


            jobQueue.push(job);
        }



        jobAvailable.notify_all();



        Utils::threadSafePrint(
            "[WASM] Novo JOB: "
            + jobId
            +
            " Height: "
            +
            std::to_string(height),
            true
        );



    }
    catch(const std::exception& e)
    {

        Utils::threadSafePrint(
            std::string(
                "[WASM] Erro criando JOB: "
            )
            +
            e.what(),
            true
        );

    }
}



}
