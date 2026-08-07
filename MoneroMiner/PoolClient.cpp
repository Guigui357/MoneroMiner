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

extern void startMiningWorkers();

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

    if (!event || !event->data || event->numBytes <= 0)
    {
        Utils::threadSafePrint(
            "[WASM] Mensagem WebSocket vazia",
            true
        );

        return EM_TRUE;
    }

    std::string msg(
        reinterpret_cast<const char*>(event->data),
        event->numBytes
    );

    Utils::threadSafePrint(
        "[WASM] RX: " + msg,
        true
    );

    // ==================================================
    // Parse JSON
    // ==================================================

    picojson::value json;

    std::string err =
        picojson::parse(json, msg);

    if (!err.empty())
    {
        Utils::threadSafePrint(
            "[WASM] JSON invalido: " + err,
            true
        );

        return EM_TRUE;
    }

    if (!json.is<picojson::object>())
    {
        Utils::threadSafePrint(
            "[WASM] JSON recebido nao e objeto",
            true
        );

        return EM_TRUE;
    }

    picojson::object obj =
        json.get<picojson::object>();


    // ==================================================
    // LOGIN RESPONSE
    // ==================================================

    auto resultIt = obj.find("result");

    if (resultIt != obj.end() &&
        resultIt->second.is<picojson::object>())
    {
        picojson::object result =
            resultIt->second.get<picojson::object>();

        auto statusIt = result.find("status");

        if (statusIt != result.end() &&
            statusIt->second.is<std::string>())
        {
            std::string status =
                statusIt->second.get<std::string>();

            Utils::threadSafePrint(
                "[WASM] Pool status: " + status,
                true
            );

            if (status == "OK")
            {
                Utils::threadSafePrint(
                    "[WASM] *** LOGIN ACEITO ***",
                    true
                );
            }
        }
    }


    // ==================================================
    // JOB
    //
    // Formato recebido:
    //
    // {
    //   "jsonrpc":"2.0",
    //   "method":"job",
    //   "params":{
    //      "blob":"...",
    //      "algo":"rx/0",
    //      "height":123,
    //      "seed_hash":"...",
    //      "job_id":"...",
    //      "target":"..."
    //   }
    // }
    // ==================================================

    auto methodIt = obj.find("method");

    if (methodIt != obj.end() &&
        methodIt->second.is<std::string>())
    {
        std::string method =
            methodIt->second.get<std::string>();


        // ----------------------------------------------
        // JOB
        // ----------------------------------------------

        if (method == "job")
        {
            Utils::threadSafePrint(
                "[WASM] *** JOB RECEBIDO ***",
                true
            );

            auto paramsIt =
                obj.find("params");

            if (paramsIt == obj.end())
            {
                Utils::threadSafePrint(
                    "[WASM] JOB sem params",
                    true
                );

                return EM_TRUE;
            }

            if (!paramsIt->second.is<picojson::object>())
            {
                Utils::threadSafePrint(
                    "[WASM] params do JOB nao e objeto",
                    true
                );

                return EM_TRUE;
            }

            picojson::object params =
                paramsIt->second.get<picojson::object>();


            // ------------------------------------------
            // Debug dos campos
            // ------------------------------------------

            if (params.find("job_id") != params.end())
            {
                Utils::threadSafePrint(
                    "[WASM] Job ID: " +
                    params["job_id"].get<std::string>(),
                    true
                );
            }

            if (params.find("height") != params.end())
            {
                Utils::threadSafePrint(
                    "[WASM] Height: " +
                    std::to_string(
                        static_cast<uint64_t>(
                            params["height"].get<double>()
                        )
                    ),
                    true
                );
            }

            if (params.find("algo") != params.end())
            {
                Utils::threadSafePrint(
                    "[WASM] Algo: " +
                    params["algo"].get<std::string>(),
                    true
                );
            }

            if (params.find("target") != params.end())
            {
                Utils::threadSafePrint(
                    "[WASM] Target: " +
                    params["target"].get<std::string>(),
                    true
                );
            }


            // ------------------------------------------
            // Criar Job
            // ------------------------------------------

            processNewJob(params);

            return EM_TRUE;
        }


        // ----------------------------------------------
        // OUTROS METODOS
        // ----------------------------------------------

        Utils::threadSafePrint(
            "[WASM] Metodo recebido: " + method,
            true
        );
    }


    // ==================================================
    // ERROR
    // ==================================================

    auto errorIt = obj.find("error");

    if (errorIt != obj.end())
    {
        Utils::threadSafePrint(
            "[WASM] Pool retornou ERROR",
            true
        );

        if (errorIt->second.is<picojson::object>())
        {
            picojson::object error =
                errorIt->second.get<picojson::object>();

            auto messageIt =
                error.find("message");

            if (messageIt != error.end() &&
                messageIt->second.is<std::string>())
            {
                Utils::threadSafePrint(
                    "[WASM] Erro: " +
                    messageIt->second.get<std::string>(),
                    true
                );
            }
        }
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

void processNewJob(const picojson::object& jobObj)
{
    try
    {
        // ==================================================
        // EXTRAI OS DADOS DO JOB
        // ==================================================

        std::string blob;
        std::string jobId;
        std::string target;
        std::string seedHash;
        uint64_t height = 0;

        // blob
        auto blobIt = jobObj.find("blob");
        if (blobIt == jobObj.end() ||
            !blobIt->second.is<std::string>())
        {
            Utils::threadSafePrint(
                "[WASM] JOB invalido: blob ausente",
                true
            );
            return;
        }

        blob = blobIt->second.get<std::string>();

        // job_id
        auto jobIdIt = jobObj.find("job_id");
        if (jobIdIt == jobObj.end() ||
            !jobIdIt->second.is<std::string>())
        {
            Utils::threadSafePrint(
                "[WASM] JOB invalido: job_id ausente",
                true
            );
            return;
        }

        jobId = jobIdIt->second.get<std::string>();

        // target
        auto targetIt = jobObj.find("target");
        if (targetIt != jobObj.end() &&
            targetIt->second.is<std::string>())
        {
            target = targetIt->second.get<std::string>();
        }

        // seed_hash
        auto seedIt = jobObj.find("seed_hash");
        if (seedIt != jobObj.end() &&
            seedIt->second.is<std::string>())
        {
            seedHash = seedIt->second.get<std::string>();
        }

        // height
        auto heightIt = jobObj.find("height");
        if (heightIt != jobObj.end() &&
            heightIt->second.is<double>())
        {
            height = static_cast<uint64_t>(
                heightIt->second.get<double>()
            );
        }

        // ==================================================
        // VALIDAÇÃO
        // ==================================================

        if (blob.empty())
        {
            Utils::threadSafePrint(
                "[WASM] JOB invalido: blob vazio",
                true
            );
            return;
        }

        if (jobId.empty())
        {
            Utils::threadSafePrint(
                "[WASM] JOB invalido: job_id vazio",
                true
            );
            return;
        }

        if (target.empty())
        {
            Utils::threadSafePrint(
                "[WASM] JOB invalido: target vazio",
                true
            );
            return;
        }

        if (seedHash.empty())
        {
            Utils::threadSafePrint(
                "[WASM] JOB invalido: seed_hash vazio",
                true
            );
            return;
        }

        // ==================================================
        // CRIA O OBJETO JOB
        // ==================================================

        Job job(
            blob,
            jobId,
            target,
            height,
            seedHash
        );

        // ==================================================
        // COLOCA O JOB NA FILA
        // ==================================================

        {
            std::lock_guard<std::mutex> lock(jobMutex);

            // O Job antigo fica inválido.
            while (!jobQueue.empty())
                jobQueue.pop();

            jobQueue.push(job);
        }

        // ==================================================
        // ATUALIZA ESTADO DA POOL
        // ==================================================

        currentSeedHash = seedHash;
        currentTargetHex = target;

        // ==================================================
        // ACORDA THREADS ESPERANDO POR JOB
        // ==================================================

        jobAvailable.notify_all();
        jobQueueCondition.notify_all();

        // ==================================================
        // LOG
        // ==================================================

        Utils::threadSafePrint(
            "[WASM] Novo JOB recebido: " +
            jobId +
            " | Height: " +
            std::to_string(height) +
            " | Target: " +
            target,
            true
        );

        // ==================================================
        // INICIA OS WORKERS NO PRIMEIRO JOB
        // ==================================================

        static std::atomic<bool> workersStarted(false);

        if (!workersStarted.exchange(true))
        {
            Utils::threadSafePrint(
                "[WASM] Primeiro Job recebido. "
                "Iniciando startMiningWorkers()...",
                true
            );

            startMiningWorkers();

            Utils::threadSafePrint(
                "[WASM] startMiningWorkers() concluido.",
                true
            );
        }
    }
    catch (const std::exception& e)
    {
        Utils::threadSafePrint(
            std::string("[WASM] Erro criando JOB: ") +
            e.what(),
            true
        );
    }
    catch (...)
    {
        Utils::threadSafePrint(
            "[WASM] Erro desconhecido criando JOB",
            true
        );
    }
}


}
