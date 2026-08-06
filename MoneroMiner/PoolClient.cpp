#include "PoolClient.h"
#include "Config.h"
#include "Globals.h"
#include "Utils.h"
#include "RandomXManager.h"
#include "MiningStats.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <vector>
#include <atomic>
#include "picojson.h"

// Biblioteca nativa do Emscripten para controle do WebSocket do Navegador
#include <emscripten/websocket.h>

using namespace picojson;

namespace PoolClient {
    // Definições de membros estáticos
    socket_t poolSocket = INVALID_SOCKET_VALUE; 
    EMSCRIPTEN_WEBSOCKET_T wsHandle = 0;        

    std::mutex jobMutex;
    std::queue<Job> jobQueue;
    std::condition_variable jobAvailable;
    std::condition_variable jobQueueCondition;
    std::atomic<bool> shouldStop(false);
    std::string currentSeedHash;
    std::string sessionId;
    std::string currentTargetHex;
    std::vector<std::shared_ptr<MiningThreadData>> threadData;
    std::mutex socketMutex;
    std::mutex submitMutex;
    std::string poolId;

    // Elementos de sincronização assíncrona
    static std::string lastResponseStr;
    static std::mutex responseMutex;
    static std::condition_variable responseAvailable;
    static std::atomic<bool> responseReady{false};

    // Forward declarations internas
    bool sendRequest(const std::string& request);
    void processNewJobFromObj(const picojson::object& obj);
    bool processShareResponse(const std::string& response);

    // =========================================================================
    // CALLBACKS DO WEBSOCKET (Executados na Thread do Navegador)
    // =========================================================================

    EM_BOOL on_message_received(int eventType, const EmscriptenWebSocketMessageEvent *websocketEvent, void *userData) {
        (void)eventType; (void)userData;
        
        if (websocketEvent->isText && websocketEvent->numBytes > 0) {
            std::string msg((char*)websocketEvent->data, websocketEvent->numBytes);
            
            try {
                picojson::value v;
                std::string err = picojson::parse(v, msg);
                if (!err.empty()) return EM_TRUE;

                if (v.is<picojson::object>()) {
                    const picojson::object& obj = v.get<picojson::object>();

                    // CAPTURA DE JOB
                    if (obj.find("identifier") != obj.end() && obj.at("identifier").get<std::string>() == "job") {
                        processNewJobFromObj(obj);
                    } 
                    // CAPTURA DE RESPOSTAS DE SHARE OU HANDSHAKE
                    else {
                        std::lock_guard<std::mutex> lock(responseMutex);
                        lastResponseStr = msg;
                        responseReady = true;
                        responseAvailable.notify_one(); 
                    }
                }
            } catch (...) {
                // Tratamento seguro contra falhas
            }
        }
        return EM_TRUE;
    }

    EM_BOOL on_close_event(int eventType, const EmscriptenWebSocketCloseEvent *websocketEvent, void *userData) {
        (void)eventType; (void)websocketEvent; (void)userData;
        Utils::threadSafePrint("[WASM] ❌ Conexão WebSocket encerrada com o servidor proxy.", true);
        poolSocket = INVALID_SOCKET_VALUE;
        wsHandle = 0;
        return EM_TRUE;
    }

    EM_BOOL on_ws_open(int eventType, const EmscriptenWebSocketOpenEvent *websocketEvent, void *userData) {
        (void)eventType; (void)websocketEvent; (void)userData;
        
        PoolClient::poolSocket = 1; 
        Utils::threadSafePrint("[WASM] -> SUCESSO: WebSocket conectado e pronto para tráfego!", true);
        
        PoolClient::login(config.walletAddress, config.password, config.workerName, config.userAgent);
        return EM_TRUE;
    }

    // =========================================================================
    // IMPLEMENTAÇÃO DAS FUNÇÕES CORE DE REDE
    // =========================================================================

    bool initialize() {
        return true; 
    }

    bool connect() {
        if (!emscripten_websocket_is_supported()) {
            Utils::threadSafePrint("[WASM] Erro crítico: WebSockets não são suportados neste navegador.", true);
            return false;
        }

        static const char* proxy_url = "wss://://onrender.com"; 

        Utils::threadSafePrint("[WASM] Tentando abrir WebSocket assíncrono para: " + std::string(proxy_url), true);

        EmscriptenWebSocketCreateAttributes ws_attrs;
        std::memset(&ws_attrs, 0, sizeof(ws_attrs));
        
        ws_attrs.url = proxy_url;
        ws_attrs.protocols = NULL;
        ws_attrs.createOnMainThread = EM_TRUE;

        wsHandle = emscripten_websocket_new(&ws_attrs);
        if (wsHandle <= 0) {
            Utils::threadSafePrint("[WASM] Falha ao instanciar ponte de controle WebSocket.", true);
            return false;
        }

        emscripten_websocket_set_onopen_callback(wsHandle, NULL, on_ws_open);
        emscripten_websocket_set_onmessage_callback(wsHandle, NULL, on_message_received);
        emscripten_websocket_set_onclose_callback(wsHandle, NULL, on_close_event);

        return true;
    }

    bool login(const std::string& wallet, const std::string& password, const std::string& worker, const std::string& userAgent) {
        (void)password; (void)worker; (void)userAgent;

        picojson::object loginReq;
        loginReq["identifier"] = picojson::value("handshake");
        loginReq["wallet"] = picojson::value(wallet);
        
        std::string payload = picojson::value(loginReq).serialize();
        
        std::lock_guard<std::mutex> lock(socketMutex);
        if (wsHandle <= 0) return false;

        EMSCRIPTEN_RESULT res = emscripten_websocket_send_utf8_text(wsHandle, payload.c_str());
        
        if (res == EMSCRIPTEN_RESULT_SUCCESS) {
            Utils::threadSafePrint("[WASM] Handshake de autenticação disparado para o Render.", true);
            sessionId = "wasm_active_session"; 
            return true;
        }
        return false;
    }

    bool sendRequest(const std::string& request) {
        if (poolSocket == INVALID_SOCKET_VALUE || wsHandle <= 0) {
            return false;
        }
        EMSCRIPTEN_RESULT res = emscripten_websocket_send_utf8_text(wsHandle, request.c_str());
        return (res == EMSCRIPTEN_RESULT_SUCCESS);
    }

    std::string sendAndReceive(const std::string& payload) {
        std::unique_lock<std::mutex> lock(responseMutex);
        responseReady = false;
        lastResponseStr.clear();

        if (!sendRequest(payload)) {
            return "";
        }

        if (responseAvailable.wait_for(lock, std::chrono::seconds(4), [] { return responseReady.load(); })) {
            return lastResponseStr;
        }
        return ""; 
    }

    // =========================================================================
    // PROCESSAMENTO DE JOBS E SHARES
    // =========================================================================

    void processNewJobFromObj(const picojson::object& obj) {
        try {
            std::string blobStr = obj.at("blob").get<std::string>();
            std::string jobId = obj.at("job_id").get<std::string>();
            std::string target = obj.at("target").get<std::string>();
            uint64_t height = static_cast<uint64_t>(obj.at("height").get<double>());
            std::string seedHash = obj.at("seed_hash").get<std::string>();

            if (RandomXManager::setTargetAndDifficulty(target)) {
                Job job(blobStr, jobId, target, height, seedHash);
                
                {
                    std::lock_guard<std::mutex> lock(jobMutex);
                    jobQueue.push(job);
                    jobAvailable.notify_all();
                }
                
                Utils::threadSafePrint("[WASM] -> SUCESSO: Novo Job recebido do Proxy! ID: " + jobId, true);

                if (::miningThreads.empty() && !shouldStop) {
                    Utils::threadSafePrint("[WASM] Inicializando a máquina virtual RandomX (Modo Light)...", true);
                    
                    if (!RandomXManager::initialize(seedHash)) {
                        Utils::threadSafePrint("[WASM] Falha crítica ao inicializar gerência do RandomX.", true);
                        return;
                    }

                    threadData.resize(static_cast<size_t>(config.numThreads));
                    for (size_t i = 0; i < static_cast<size_t>(config.numThreads); i++) {
                        threadData[i] = std::make_shared<MiningThreadData>(static_cast<int>(i));
                        if (!threadData[i]->initializeVM()) {
                            Utils::threadSafePrint("[WASM] Falha ao alocar VM para o Worker " + std::to_string(i), true);
                            return;
                        }
                    }

                    for (size_t i = 0; i < static_cast<size_t>(config.numThreads); i++) {
                        ::miningThreads.emplace_back(miningThread, threadData[i].get());
                    }
                }

                if (!::statsThreadRunning) {
                    ::statsWebThread = std::thread(webStatsMonitorLoop);
                }

                Utils::threadSafePrint("[WASM] === WORKERS DISPARADOS COM SUCESSO! MINERAÇÃO ATIVA ===", true);
            }
        } 
        catch (const std::exception& e) {
            Utils::threadSafePrint("[WASM] Erro ao analisar propriedades do Job: " + std::string(e.what()), true);
        }
    }

    bool submitShare(const std::string& jobId, const std::string& nonceHex,
                     const std::string& hashHex, const std::string& algo) {
        (void)algo;
        std::lock_guard<std::mutex> submitLock(submitMutex);

        picojson::object submitReq;
        submitReq["identifier"] = picojson::value("submit");
        submitReq["job_id"] = picojson::value(jobId);
        submitReq["nonce"] = picojson::value(nonceHex);
        submitReq["result"] = picojson::value(hashHex);

        std::string payload = picojson::value(submitReq).serialize();
        std::unique_lock<std::mutex> rLock(responseMutex);
        responseReady = false;

        if (!sendRequest(payload)) {
            MiningStatsUtil::rejectedShares++;
            return false;
        }

        Utils::threadSafePrint("[WASM] Compartilhamento (Share) computado enviado para o Proxy...", true);

        if (responseAvailable.wait_for(rLock, std::chrono::seconds(4), [] { return responseReady.load(); })) {
            if (lastResponseStr.find("hash") != std::string::npos) {
                MiningStatsUtil::acceptedShares++;
                Utils::threadSafePrint("[WASM] 🔥 EXCELENTE! Share validado e ACEITO pela Pool MoneroOcean!", true);
                return true;
            }
        }

        MiningStatsUtil::rejectedShares++;
        Utils::threadSafePrint("[WASM] ❌ Share REJEITADO ou sem resposta de validação.", true);
        return false;
    }

    void cleanup() {
        if (wsHandle > 0) {
            emscripten_websocket_close(wsHandle, 1000, "Sessao Encerrada");
        }
        poolSocket = INVALID_SOCKET_VALUE;
        wsHandle = 0;
    }

    // =========================================================================
    // STUBS DE COMPATIBILIDADE
    // =========================================================================
    void jobListener() {}

    void processNewJob(const picojson::object& jobObj) {
        (void)jobObj;
    }

    void distributeJob(const Job& job) {
        (void)job;
    }

    std::string receiveData(socket_t sock) {
        (void)sock;
        return "";
    }

    std::string sendData(const std::string& data) {
        (void)data;
        return "";
    }

    bool reconnect() {
        return connect();
    }

    void sendKeepalive() {}

    bool processShareResponse(const std::string& response) {
        (void)response;
        return true;
    }
}
