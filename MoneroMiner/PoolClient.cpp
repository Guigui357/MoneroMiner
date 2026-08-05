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
#include "picojson.h"

// Biblioteca nativa do Emscripten para controle do WebSocket do Navegador
#include <emscripten/websocket.h>

using namespace picojson;

namespace PoolClient {
    // Definições de membros estáticos (Flags lógicas para controle interno)
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

    // Elementos de sincronização para emular comportamento síncrono no WebSocket assíncrono
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
        
        if (websocketEvent->isText) {
            std::string msg((char*)websocketEvent->data, websocketEvent->numBytes);
            
            try {
                picojson::value v;
                std::string err = picojson::parse(v, msg);
                if (!err.empty()) return EM_TRUE;

                const picojson::object& obj = v.get<picojson::object>();

                // 1. CAPTURA DE JOB (Sincronizado com o seu 'server.js' do Render)
                if (obj.find("identifier") != obj.end() && obj.at("identifier").get<std::string>() == "job") {
                    processNewJobFromObj(obj);
                } 
                // 2. CAPTURA DE RESPOSTAS DE SHARE OU HANDSHAKE
                else {
                    std::lock_guard<std::mutex> lock(responseMutex);
                    lastResponseStr = msg;
                    responseReady = true;
                    responseAvailable.notify_one(); // Acorda a thread de mineração que estava esperando resposta
                }
            } catch (...) {
                // Tratamento silencioso de falhas em formatos desconhecidos
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

    // =========================================================================
    // IMPLEMENTAÇÃO DAS FUNÇÕES CORE DE REDE
    // =========================================================================

    bool initialize() {
        // Inicialização lógica do subsistema na Web sempre retorna verdadeiro
        return true; 
    }

    bool connect() {
        if (!emscripten_websocket_is_supported()) {
            Utils::threadSafePrint("[WASM] Erro crítico: WebSockets não são suportados neste navegador.", true);
            return false;
        }

        // COLOQUE AQUI A URL ATIVA DO SEU PROXY NODE.JS NO RENDER
        EmscriptenWebSocketCreateAttributes ws_attrs = {
            "wss://proxy-xmr.onrender.com", 
            NULL,
            EM_TRUE
        };

        wsHandle = emscripten_websocket_new(&ws_attrs);
        if (wsHandle <= 0) {
            Utils::threadSafePrint("[WASM] Falha ao instanciar ponte de controle WebSocket.", true);
            return false;
        }

        // Vincula os callbacks de eventos assíncronos
        emscripten_websocket_set_onmessage_callback(wsHandle, NULL, on_message_received);
        emscripten_websocket_set_onclose_callback(wsHandle, NULL, on_close_event);

        // Dá uma pequena folga de tempo para o handshake inicial do protocolo WS se consolidar na Cloud
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        
        poolSocket = 1; // Ativa flag de simulação de socket aberto
        Utils::threadSafePrint("[WASM] Canal WebSocket aberto com o Proxy.", true);
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

        // Versão atualizada e infalível para o SDK moderno do Emscripten
        EMSCRIPTEN_RESULT res = emscripten_websocket_send_utf8_text(wsHandle, payload.c_str());
        
        if (res == EMSCRIPTEN_RESULT_SUCCESS) {
            Utils::threadSafePrint("[WASM] Handshake de autenticação disparado para o Render.", true);
            sessionId = "wasm_active_session"; // Stub de sessão para manter estados consistentes no motor C++
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

    // Emulação de Envio e Resposta Síncrona exigida pelo seu minerador legado
    std::string sendAndReceive(const std::string& payload) {
        std::unique_lock<std::mutex> lock(responseMutex);
        responseReady = false;
        lastResponseStr.clear();

        if (!sendRequest(payload)) {
            return "";
        }

        // Dorme temporariamente a thread por até 4 segundos aguardando o callback do navegador acordá-la
        if (responseAvailable.wait_for(lock, std::chrono::seconds(4), [] { return responseReady.load(); })) {
            return lastResponseStr;
        }

        return ""; // Retorna vazio em caso de timeout
    }

    // =========================================================================
    // PROCESSAMENTO DE TRABALHOS (JOBS) E PARTICIPAÇÕES (SHARES)
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
                
                std::lock_guard<std::mutex> lock(jobMutex);
                jobQueue.push(job);
                jobAvailable.notify_one(); // Libera as threads de mineração que aguardavam Jobs
                
                Utils::threadSafePrint("[WASM] -> SUCESSO: Novo Job recebido e aceito no motor! ID: " + jobId, true);
            }
        } catch (const std::exception& e) {
            Utils::threadSafePrint("[WASM] Erro ao analisar propriedades do Job: " + std::string(e.what()), true);
        }
    }

    bool submitShare(const std::string& jobId, const std::string& nonceHex, const std::string& hashHex, const std::string& algo) {
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

        // Bloqueia e aguarda a resposta do server.js ("hash" de sucesso ou erro)
        if (responseAvailable.wait_for(rLock, std::chrono::seconds(4), [] { return responseReady.load(); })) {
            if (lastResponseStr.find("hash") != std::string::npos) {
                MiningStatsUtil::acceptedShares++;
                Utils::threadSafePrint("[WASM] 🔥 EXCELENTE! Share validado e ACEITO pela Pool MoneroOcean!", true);
                return true;
            }
        }

        MiningStatsUtil::rejectedShares++;
        Utils::threadSafePrint("[WASM] ❌ Share REJEITADO ou sem resposta de validação dentro do limite de tempo.", true);
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
    // STUBS DE COMPATIBILIDADE (Evitam erros de 'Undefined Symbol' no Linker)
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
}
