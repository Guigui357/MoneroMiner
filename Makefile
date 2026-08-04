# MoneroMiner - WebAssembly (Emscripten) Makefile
# Compiles RandomX library and MoneroMiner for Web environments

# Substituição para o ecossistema Emscripten
CXX = em++
CC = emcc

# Flags de otimização pesada para WebAssembly (-O3 e LTO são suportados e recomendados)
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -flto
CFLAGS = -O3 -Wall -Wextra -flto

# FLAGS ESPECÍFICAS DO EMSCRIPTEN (Cruciais para o navegador)
# 1. ALLOW_MEMORY_GROWTH=1: Permite expandir a RAM alocada para o Scratchpad do RandomX.
# 2. EXPORTED_FUNCTIONS: Expõe os pontos de entrada do minerador para o seu index.html chamar.
# 3. EXTRA_EXPORTED_RUNTIME_METHODS: Permite envelopar tipos usando cwrap/ccall.
EMSCRIPTEN_FLAGS = -s WASM=1 \
                   -s ALLOW_MEMORY_GROWTH=1 \
                   -s TOTAL_MEMORY=134217728 \
                   -s EXPORTED_FUNCTIONS="['_startMining', '_stopMining', '_cryptonight_hash', '_randomx_hash_run']" \
                   -s EXTRA_EXPORTED_RUNTIME_METHODS="['ccall', 'cwrap']"

# Concatena as flags do Emscripten ao Linker
LDFLAGS = -flto $(EMSCRIPTEN_FLAGS)

# Directories
SRC_DIR = MoneroMiner
BUILD_DIR = build
BIN_DIR = bin

# Detect RandomX source directory
RANDOMX_DIR := $(shell if [ -d RandomX ]; then echo RandomX; elif [ -d randomx ]; then echo randomx; else echo ""; fi)
ifeq ($(RANDOMX_DIR),)
$(error RandomX source directory not found. Looked for 'RandomX' and 'randomx' in $(PWD))
endif
RANDOMX_BUILD = $(RANDOMX_DIR)/build

# Absolute RandomX source path and cache file
RANDOMX_SRC_ABS := $(shell cd $(RANDOMX_DIR) && pwd)
RANDOMX_CACHE := $(RANDOMX_BUILD)/CMakeCache.txt

# Include paths
INCLUDES = -I$(SRC_DIR) -I$(RANDOMX_DIR)/src

# Source files (Excluindo arquivos específicos do Windows e componentes nativos de Sockets TCP de rede)
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
SOURCES := $(filter-out $(SRC_DIR)/framework.cpp \
                        $(SRC_DIR)/pch.cpp \
                        $(SRC_DIR)/main.cpp \
                        $(SRC_DIR)/MiningThread.cpp \
                        $(SRC_DIR)/Platform.cpp \
                        $(SRC_DIR)/Utils.cpp \
                        $(SRC_DIR)/PoolClient.cpp, $(SOURCES))

# Object files
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

# MUDANÇA DE DESTINO: Agora a saída gera a interface JavaScript (.js) e o binário WebAssembly (.wasm)
TARGET = $(BIN_DIR)/miner.js

# RandomX library
RANDOMX_LIB = $(RANDOMX_BUILD)/librandomx.a

# Default target
all: directories randomx $(TARGET)

# Create build directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(RANDOMX_BUILD)

# Build RandomX library usando emcmake (COM CORREÇÃO PARA DESATIVAR TESTES/BENCHMARKS)
randomx: directories
	@echo "Building RandomX library for WebAssembly..."
	@if [ ! -d "$(RANDOMX_DIR)" ]; then \
		echo "Error: RandomX source directory '$(RANDOMX_DIR)' not found."; \
		exit 1; \
	fi
	@if [ -f "$(RANDOMX_CACHE)" ]; then \
		old_src=$$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$(RANDOMX_CACHE)" | tr -d '\r'); \
		if [ -n "$$old_src" ] && [ "$$old_src" != "$(RANDOMX_SRC_ABS)" ]; then \
			rm -rf "$(RANDOMX_BUILD)"; \
			mkdir -p "$(RANDOMX_BUILD)"; \
		fi; \
	fi
	# CORREÇÃO: Adicionado -DBUILD_BENCHMARK=OFF e -DBUILD_TESTS=OFF para evitar o erro de pthread_setaffinity_np
	@cd "$(RANDOMX_DIR)" && mkdir -p build && cd build && \
	emcmake cmake -DCMAKE_BUILD_TYPE=Release \
	        -DBUILD_SHARED_LIBS=OFF \
	        -DBUILD_BENCHMARK=OFF \
	        -DBUILD_TESTS=OFF \
	        -DARCH=generic \
	        .. && \
	$(MAKE) -j$(nproc)
	@echo "RandomX static library compiled into WASM bytecode successfully"

# Build MoneroMiner (.js + .wasm)
$(TARGET): $(OBJECTS)
	@echo "Linking WebAssembly Target $(TARGET)..."
	$(CXX) $(OBJECTS) $(RANDOMX_LIB) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete! Output files generated in: $(BIN_DIR)/miner.js e $(BIN_DIR)/miner.wasm"

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling WebAssembly Object $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf build/*.o
	rm -f bin/miner.js bin/miner.wasm

distclean: clean
	@echo "Deep cleaning..."
	rm -rf randomx/build
	rm -rf RandomX/build
	rm -rf bin

rebuild: distclean all

info:
	@echo "Compiler: $(CXX)"
	@echo "Flags: $(CXXFLAGS)"
	@echo "Emscripten Flags: $(EMSCRIPTEN_FLAGS)"
	@echo "Output Target: $(TARGET)"
