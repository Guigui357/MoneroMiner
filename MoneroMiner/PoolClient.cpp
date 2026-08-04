#include "PoolClient.h"
#include "Config.h"
#include "Globals.h"
#include "Utils.h"
#include "RandomXManager.h"
#include "MiningStats.h"
#include <iostream>
#include <chrono>
#include <thread>
#include "picojson.h"

// Biblioteca nativa do Emscripten para controle de WebSockets do Navegador
#include <emscripten/websocket.h>

using namespace picojson;

namespace PoolClient {
    socket_t poolSocket = INVALID_SOCKET_VALUE; // Mantido como flag (1 = Conectado, -1 = Desconectado)
    EMSCRIPTEN_WEBSOCKET_T wsHandle = 0;        // O ID real do WebSocket na Web API

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

    // Sincronizadores para emular chamadas de Envio/Resposta solicitadas pelo minerador
    static std::string lastResponseStr;
    static std::mutex responseMutex;
    static std::condition_variable responseAvailable;
    static std::atomic<bool> responseReady{false};

    // --- CALLBACK: Executado toda vez que o seu server.js envia dados ---
    EM_BOOL on_message_received(int eventType, const EmscriptenWebSocketMessageEvent *websocketEvent, void *userData) {
        (void)eventType; (void)userData;
        
        if (websocketEvent->isText) {
            std::string msg((char*)websocketEvent->data, websocketEvent->numBytes);
            
            try {
                picojson::value v;
                std::string err = picojson::parse(v, msg);
                if (!err.empty()) return EM_TRUE;

                const picojson::object& obj = v.get<picojson::object>();

                // Detecta se a mensagem vinda do server.js é um Job Novo
                if (obj.find("identifier") != obj.end() && obj.at("identifier").get<std::string>() == "job") {
                    std::string blobStr = obj.at("blob").get<std::string>();
                    std::string jobId = obj.at("job_id").get<std::string>();
                    std::string target = obj.at("target").get<std::string>();
                    uint64_t height = static_cast<uint64_t>(obj.at("height").get<double>());
                    std::string seedHash = obj.at("seed_hash").get<std::string>();

                    if (RandomXManager::setTargetAndDifficulty(target)) {
                        Job job(blobStr, jobId, target, height, seedHash);
                        
                        std::lock_guard<std::mutex> lock(jobMutex);
                        jobQueue.push(job);
                        jobAvailable.notify_one();
                        
                        Utils::threadSafePrint(" Novo Job recebido do Proxy! ID: " + jobId, true);
                    }
                } 
                // Detecta se é uma resposta de Hash aceito ou erro (Libera a thread bloqueada em submitShare)
                else {
                    std::lock_guard<std::mutex> lock(responseMutex);
                    lastResponseStr = msg;
                    responseReady = true;
                    responseAvailable.notify_one();
                }
            } catch (...) {
                // Falha de parse segura
            }
        }
        return EM_TRUE;
    }

    EM_BOOL on_close_event(int eventType, const EmscriptenWebSocketCloseEvent *websocketEvent, void *userData) {
        (void)eventType; (void)websocketEvent; (void)userData;
        Utils::threadSafePrint("❌ Conexão WebSocket encerrada com o servidor proxy.", true);
        poolSocket = INVALID_SOCKET_VALUE;
        return EM_TRUE;
    }

    // --- FUNÇÃO CONNECT: Substitui a conexão TCP crua ---
    bool connect() {
        if (!emscripten_websocket_is_supported()) {
            Utils::threadSafePrint("WebSockets não suportados neste navegador.", true);
            return false;
        }

        // Configuração apontando para a sua instância ativa no Render
        EmscriptenWebSocketCreateAttributes ws_attrs = {
            "wss://proxy-xmr.onrender.com", // Adicione aqui a URL do seu deploy do Render
            NULL,
            EM_TRUE
        };

        wsHandle = emscripten_websocket_new(&ws_attrs);
        if (wsHandle <= 0) {
            Utils::threadSafePrint("Falha ao instanciar objeto WebSocket.", true);
            return false;
        }

        emscripten_websocket_set_onmessage_callback(wsHandle, NULL, on_message_received);
        emscripten_websocket_set_onclose_callback(wsHandle, NULL, on_close_event);

        // Aguarda estabilização assíncrona da conexão na Web
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        poolSocket = 1; // Ativa flag de conectado
        return true;
    }

    // --- FUNÇÃO DE LOGIN: Formata a mensagem exatamente como o seu server.js espera ---
    bool login(const std::string& wallet, const std::string& password, const std::string& worker, const std::string& userAgent) {
        (void)password; (void)worker; (void)userAgent;

        picojson::object loginReq;
        loginReq["identifier"] = picojson::value("handshake");
        loginReq["wallet"] = picojson::value(wallet);
        
        std::string payload = picojson::value(loginReq).serialize();
        
        std::lock_guard<std::mutex> lock(socketMutex);
        EMSCRIPTEN_RESULT res = emscripten_websocket_send_utf8_text(wsHandle, payload.c_str());
        
        if (res == EMSCRIPTEN_RESULT_SUCCESS) {
            Utils::threadSafePrint(" Handshake de Login enviado para o Proxy.", true);
            sessionId = "wasm_session"; // Cria um ID fictício para validar o estado interno do C++
            return true;
        }
        return false;
    }

    // --- FUNÇÃO SUBMIT: Envia as participações encontradas de volta ao server.js ---
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

        // Envia os dados pelo WebSocket
        emscripten_websocket_send_utf8_text(wsHandle, payload.c_str());
        Utils::threadSafePrint(" Enviando Share encontrado para validação...", true);

        // Aguarda até 4 segundos o server.js responder "hash" ou "error"
        if (responseAvailable.wait_for(rLock, std::chrono::seconds(4), [] { return responseReady.load(); })) {
            if (lastResponseStr.find("hash") != std::string::npos) {
                MiningStatsUtil::acceptedShares++;
                Utils::threadSafePrint("🔥 Share ACEITO pelo servidor!", true);
                return true;
            }
        }

        MiningStatsUtil::rejectedShares++;
        Utils::threadSafePrint("❌ Share REJEITADO ou sem resposta.", true);
        return false;
    }

    void cleanup() {
        if (wsHandle > 0) {
            emscripten_websocket_close(wsHandle, 1000, "Sessão Finalizada");
        }
        poolSocket = INVALID_SOCKET_VALUE;
    }

    // Funções nativas obsoletas na Web que foram mantidas como stubs vazios para não quebrar dependências do linker
    bool initialize() { return true; }
    void jobListener() {} 
    void distributeJob(const Job& job) { (void)job; }
    bool reconnect() { return connect(); }
    void sendKeepalive() {}
}
