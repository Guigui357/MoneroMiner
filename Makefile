# MoneroMiner - WebAssembly (Emscripten) Makefile
# Compiles RandomX library and MoneroMiner for Web environments

CXX = em++
CC = emcc

CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -flto
CFLAGS = -O3 -Wall -Wextra -flto

EMSCRIPTEN_FLAGS = -s WASM=1 \
                   -s ALLOW_MEMORY_GROWTH=1 \
                   -s TOTAL_MEMORY=134217728 \
                   -s EXPORTED_FUNCTIONS="['_startMining', '_stopMining', '_cryptonight_hash', '_randomx_hash_run']" \
                   -s EXTRA_EXPORTED_RUNTIME_METHODS="['ccall', 'cwrap']"

LDFLAGS = -pthread -flto $(EMSCRIPTEN_FLAGS)

# Directories
SRC_DIR = MoneroMiner
BUILD_DIR = build
BIN_DIR = bin

RANDOMX_DIR := $(shell if [ -d RandomX ]; then echo RandomX; elif [ -d randomx ]; then echo randomx; else echo ""; fi)
ifeq ($(RANDOMX_DIR),)
$(error RandomX source directory not found. Looked for 'RandomX' and 'randomx' in $(PWD))
endif
RANDOMX_BUILD = $(RANDOMX_DIR)/build
RANDOMX_SRC_ABS := $(shell cd $(RANDOMX_DIR) && pwd)
RANDOMX_CACHE := $(RANDOMX_BUILD)/CMakeCache.txt

INCLUDES = -I$(SRC_DIR) -I$(RANDOMX_DIR)/src

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
SOURCES := $(filter-out $(SRC_DIR)/framework.cpp \
                        $(SRC_DIR)/pch.cpp \
                        $(SRC_DIR)/main.cpp \
                        $(SRC_DIR)/MiningThread.cpp \
                        $(SRC_DIR)/Platform.cpp \
                        $(SRC_DIR)/Utils.cpp \
                        $(SRC_DIR)/PoolClient.cpp, $(SOURCES))

OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

TARGET = $(BIN_DIR)/miner.js
RANDOMX_LIB = $(RANDOMX_BUILD)/librandomx.a

# =========================================================================
# REGRAS DE COMPILAÇÃO E DEPENDÊNCIA (CORRIGIDAS)
# =========================================================================

# Alvo principal
all: directories randomx $(TARGET)

# Cria as pastas iniciais
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(RANDOMX_BUILD)

# Compila os arquivos objetos locais (.o) OBRIGATORIAMENTE após a biblioteca randomx existir
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(RANDOMX_LIB)
	@echo "Compiling WebAssembly Object $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Gera o arquivo binário final miner.js e miner.wasm
$(TARGET): $(OBJECTS) $(RANDOMX_LIB)
	@echo "Linking WebAssembly Target $(TARGET)..."
	$(CXX) $(OBJECTS) $(RANDOMX_LIB) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete! Output generated in: $(BIN_DIR)/"

# Compilação limpa do RandomX isolando testes e benchmarks diretamente nas flags do sub-make
randomx: directories
	@echo "Building RandomX library for WebAssembly..."
	@if [ -f "$(RANDOMX_CACHE)" ]; then \
		old_src=$$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$(RANDOMX_CACHE)" | tr -d '\r'); \
		if [ -n "$$old_src" ] && [ "$$old_src" != "$(RANDOMX_SRC_ABS)" ]; then \
			rm -rf "$(RANDOMX_BUILD)"; \
			mkdir -p "$(RANDOMX_BUILD)"; \
		fi; \
	fi
	@cd "$(RANDOMX_DIR)" && mkdir -p build && cd build && \
	emcmake cmake -DCMAKE_BUILD_TYPE=Release \
	        -DBUILD_SHARED_LIBS=OFF \
	        -DARCH=generic \
	        .. && \
	$(MAKE) randomx -j$(nproc)
