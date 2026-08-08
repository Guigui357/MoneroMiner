# MoneroMiner - WebAssembly (Emscripten) Makefile
# Compiles RandomX library and MoneroMiner for Web environments

CXX = em++
CC = emcc

# Optimize both the miner and RandomX. LTO lets LLVM optimize across the
# MoneroMiner/RandomX boundary; SIMD is required by the browser build.
CXXFLAGS = -std=c++17 -O3 -flto -Wall -Wextra -pthread -msimd128 -DEMSCRIPTEN
CFLAGS = -O3 -flto -Wall -Wextra -pthread -msimd128 -DEMSCRIPTEN

EMSCRIPTEN_FLAGS = \
    -s WASM=1 \
    -s USE_PTHREADS=1 \
    -s PTHREAD_POOL_SIZE=6 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=1073741824 \
    -s MAXIMUM_MEMORY=2147483648 \
    -s ENVIRONMENT="web,worker" \
    -s EXPORTED_FUNCTIONS="['_startMining','_stopMining','_main']" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','wasmMemory']" \
    -lwebsocket.js \
    -s SINGLE_FILE=1

LDFLAGS = $(EMSCRIPTEN_FLAGS) -flto -O3

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
                        $(SRC_DIR)/main.cpp, $(SOURCES))

OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
TARGET = $(BIN_DIR)/miner.js
RANDOMX_LIB = $(RANDOMX_BUILD)/librandomx.a

all: directories
	$(MAKE) randomx
	$(MAKE) $(TARGET)

directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(RANDOMX_BUILD)

randomx:
	@echo "Building optimized RandomX library for WebAssembly..."
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
	        -DCMAKE_C_FLAGS_RELEASE="-O3 -flto -msimd128" \
	        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -flto -msimd128" \
	        -DCMAKE_EXE_LINKER_FLAGS="-flto" \
	        -DCMAKE_SHARED_LINKER_FLAGS="-flto" \
	        -DARCH=generic \
	        .. && \
	$(MAKE) randomx -j$(shell nproc 2>/dev/null || echo 4)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling optimized WebAssembly Object $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJECTS)
	@echo "Linking optimized WebAssembly Target $(TARGET)..."
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
	@echo "Pthreads: enabled"
	@echo "Pthread pool: 6"
	@echo "LTO: enabled"
	@echo "WASM SIMD: enabled"

.PHONY: all directories randomx clean distclean rebuild info