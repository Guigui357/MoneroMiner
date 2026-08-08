#include "Config.h"
#include "Globals.h"
#include "Platform.h"
#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <fstream>
#include "Utils.h"

Config::Config() {
    setDefaults();
}

void Config::setDefaults() {
    poolAddress = "xmr-us-east1.nanopool.org";
    poolPort = 10300;
    walletAddress = "8C6hFb4Buo6dYwJiZEaFhyYhZTJaR4NyXSBzKMF1BnNKMGD92yeaY3a9PxuWp9bhTAh6dAXwqyyLfFxaPRct7j81L8t4i4K2";
    workerName = "worker1";
    password = "x";
    userAgent = "MoneroMiner/1.0.0";

#ifdef __EMSCRIPTEN__
    // Browser WASM: std::thread::hardware_concurrency() is frequently
    // reported as 1 or otherwise conservatively by the browser runtime.
    // Use the Emscripten pthread pool instead of under-utilizing it.
    numThreads = 4;
#else
    numThreads = 1;
#endif

    debugMode = false;
    useLogFile = false;
    logFileName = "monerominer.log";
    headlessMode = false;
}

bool Config::parseCommandLine(int argc, char* argv[]) {
    bool threadCountSpecified = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        }
        else if (arg == "--debug") {
            debugMode = true;
        }
        else if (arg == "--logfile") {
            useLogFile = true;
        }
        else if (arg == "--threads" && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
            threadCountSpecified = true;
        }
        else if (arg == "--pool" && i + 1 < argc) {
            std::string poolStr = argv[++i];
            size_t colonPos = poolStr.find(':');
            if (colonPos != std::string::npos) {
                poolAddress = poolStr.substr(0, colonPos);
                poolPort = std::stoi(poolStr.substr(colonPos + 1));
            } else {
                poolAddress = poolStr;
            }
        }
        else if (arg == "--wallet" && i + 1 < argc) {
            walletAddress = argv[++i];
        }
        else if (arg == "--worker" && i + 1 < argc) {
            workerName = argv[++i];
        }
        else if (arg == "--password" && i + 1 < argc) {
            password = argv[++i];
        }
        else if (arg == "--headless") {
            headlessMode = true;
            useLogFile = true;
        }
    }

#ifndef __EMSCRIPTEN__
    if (!threadCountSpecified && numThreads <= 1) {
        unsigned int logicalProcessors = Platform::getLogicalProcessors();
        unsigned int recommended = (logicalProcessors > 1) ? (logicalProcessors - 1) : 1;
        numThreads = static_cast<int>(recommended);
        std::cout << "Auto-detected " << logicalProcessors
                  << " logical processors, using " << numThreads
                  << " mining threads (leaving 1 thread for system)" << std::endl;
    }
#else
    if (!threadCountSpecified && numThreads < 1)
        numThreads = 1;
#endif

    if (workerName.empty() || workerName == "worker1") {
        std::string computerName = Platform::getComputerName();
        workerName = computerName;
        for (char& c : workerName) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            else c = std::tolower(static_cast<unsigned char>(c));
        }
    }

    return true;
}

bool validateConfig(const Config& config) {
    if (config.walletAddress.empty()) {
        std::cerr << "Error: Wallet address is required" << std::endl;
        return false;
    }
    if (config.numThreads <= 0) {
        std::cerr << "Error: Invalid thread count" << std::endl;
        return false;
    }
    if (config.poolPort <= 0) {
        std::cerr << "Error: Invalid pool port" << std::endl;
        return false;
    }
    return true;
}

void Config::printConfig() const {
    std::cout << "Current configuration:" << std::endl;
    std::cout << "Pool address: " << poolAddress << ":" << poolPort << std::endl;
    std::cout << "Wallet: " << walletAddress << std::endl;
    std::cout << "Worker name: " << workerName << std::endl;
    std::cout << "User agent: " << userAgent << std::endl;
    std::cout << "Number of threads: " << numThreads << std::endl;
    std::cout << "Debug mode: " << (debugMode ? "enabled" : "disabled") << std::endl;
    std::cout << "Log file: " << logFileName << std::endl;
    std::cout << std::endl;
}

void Config::printUsage() const {
    std::cout << "MoneroMiner - Monero CPU Miner" << std::endl;
    std::cout << "\nUsage: MoneroMiner [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --help                 Show this help message" << std::endl;
    std::cout << "  --debug                Enable debug output" << std::endl;
    std::cout << "  --logfile              Enable logging to file" << std::endl;
    std::cout << "  --threads N            Number of mining threads" << std::endl;
    std::cout << "  --pool ADDRESS:PORT    Pool address and port" << std::endl;
    std::cout << "  --wallet ADDRESS       Your Monero wallet address" << std::endl;
    std::cout << "  --worker NAME          Worker name" << std::endl;
    std::cout << "  --password PASS        Pool password (default: x)" << std::endl;
    std::cout << "  --headless             Enable headless mode (no GUI)" << std::endl;
    std::cout << "\nExample:" << std::endl;
    std::cout << "  MoneroMiner.exe --wallet YOUR_WALLET --threads 4" << std::endl;
}