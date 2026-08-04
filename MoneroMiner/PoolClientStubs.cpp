#include <string>
#include <queue>
#include <mutex>

// Stub para simular as estruturas do PoolClient exigidas pela matemática interna do minerador
namespace PoolClient {
    // Declarações estáticas vazias para o compilador encontrar na memória
    std::mutex jobMutex;
    std::queue<std::string> jobQueue; // ou o tipo exato correspondente do Job se necessário

    // Deixa o gatilho de submissão de hashes vazio (o envio será feito pelo JS do navegador)
    void submitShare(const std::string& jobId, const std::string& nonceHex, const std::string& resultHex) {
        // No op (vazio) para WebAssembly
    }
}
