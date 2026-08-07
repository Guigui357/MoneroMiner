# MoneroMiner - WebAssembly (Emscripten) Makefile
# Compiles RandomX library and MoneroMiner for Web environments

CXX = em++
CC = emcc

# Otimizações pesadas para WebAssembly e SIMD de 128 bits para o RandomX
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread -msimd128 -DEMSCRIPTEN
CFLAGS = -O3 -Wall -Wextra -pthread -msimd128 -DEMSCRIPTEN

EMSCRIPTEN_FLAGS = \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=536870912 \
    -s SHARED_MEMORY=1 \
    -s ENVIRONMENT="web,worker" \
    -s EXPORTED_FUNCTIONS="['_startMining','_stopMining','_main']" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','wasmMemory']"

LDFLAGS = -pthread $(EMSCRIPTEN_FLAGS)

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

# CORREÇÃO: Include paths corrigidos (removido o arquivo .h do escopo de inclusão de diretório)
INCLUDES = -I$(SRC_DIR) -I$(RANDOMX_DIR)/src

# CORREÇÃO: Mantemos o PoolClient.cpp e MiningThread.cpp originais/adaptados na compilação.
# Removemos apenas o framework nativo do Windows/Linux (pch, framework e main legada).
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
SOURCES := $(filter-out $(SRC_DIR)/framework.cpp \
                        $(SRC_DIR)/pch.cpp \
                        $(SRC_DIR)/main.cpp, $(SOURCES))

OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

TARGET = $(BIN_DIR)/miner.js
RANDOMX_LIB = $(RANDOMX_BUILD)/librandomx.a

# =========================================================================
# REGRAS DE COMPILAÇÃO E DEPENDÊNCIA SEQUENCIAL
# =========================================================================

all: directories
	$(MAKE) randomx
	$(MAKE) $(TARGET)

directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(RANDOMX_BUILD)

# Garante que o CMake do RandomX compile em modo genérico e portátil para browsers
randomx:
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
	$(MAKE) randomx -j$(shell nproc 2>/dev/null || echo 4)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling WebAssembly Object $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJECTS)
	@echo "Linking WebAssembly Target $(TARGET)..."
	$(CXX) $(OBJECTS) $(RANDOMX_LIB) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete! Output generated successfully in: $(BIN_DIR)/"

clean:
	@echo "Cleaning build artifacts..."
	rm -rf build/*.o
	rm -f bin/miner.js bin/miner.wasm bin/miner.worker.js

distclean: clean
	@echo "Deep cleaning..."
	rm -rf randomx/build
	rm -rf RandomX/build
	rm -rf bin

rebuild: distclean all

info:
	@echo "Compiler: $(CXX)"
	@echo "Output Target: $(TARGET)"

.PHONY: all directories randomx clean distclean rebuild info
