# MoneroMiner - WebAssembly (Emscripten) Makefile
# Compiles RandomX library and MoneroMiner for Web environments

CXX = em++
CC = emcc

# Optimize both the miner and RandomX. LTO lets LLVM optimize across the
# MoneroMiner/RandomX boundary; SIMD is required by the browser build.
CXXFLAGS = -std=c++17 -O3 -flto -Wall -Wextra -pthread -msimd128 -DEMSCRIPTEN
CFLAGS = -O3 -flto -Wall -Wextra -pthread -msimd128 -DEMSCRIPTEN

# RandomX Fast mode needs the full dataset (~2.08 GiB). The old 2 GiB
# Emscripten ceiling was too small once the dataset and the rest of the heap
# were combined, so allow a 3 GiB wasm32 heap.
EMSCRIPTEN_FLAGS = \
    -s WASM=1 \
    -s USE_PTHREADS=1 \
    -s PTHREAD_POOL_SIZE=6 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=268435456 \
    -s MAXIMUM_MEMORY=3221225472 \
    -s ENVIRONMENT="web,worker" \
    -s EXPORTED_FUNCTIONS="['_startMining','_stopMining','_main']" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','allocateUTF8']" \
    -lwebsocket.js \
    -s SINGLE_FILE=1

LDFLAGS = $(EMSCRIPTEN_FLAGS) -O3 -flto

SRC_DIR = MoneroMiner
BUILD_DIR = build
BIN_DIR = bin

RANDOMX_DIR := $(shell if [ -d RandomX ]; then echo RandomX; elif [ -d randomx ]; then echo randomx; else echo ""; fi)
ifeq ($(RANDOMX_DIR),)
$(error RandomX source directory not found. Looked for 'RandomX' and 'randomx' in $(PWD))
endif
RANDOMX_BUILD = $(RANDOMX_DIR)/build-wasm
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
			echo "RandomX source changed, cleaning build..."; \
			rm -rf "$(RANDOMX_BUILD)"; \
		fi; \
	fi
	@mkdir -p "$(RANDOMX_BUILD)"
	@cd "$(RANDOMX_DIR)" && \
		emcmake cmake -S . -B build-wasm \
			-DCMAKE_BUILD_TYPE=Release \
			-DRANDOMX_WASM_SIMD=ON \
			-DRANDOMX_WASM_PTHREADS=ON
	@cmake --build "$(RANDOMX_BUILD)" -j$(shell nproc)

# RandomXManager.cpp historically forced LIGHT mode whenever __EMSCRIPTEN__
# was defined. For this one translation unit, build a Fast-mode variant
# instead of replacing the full source file. Other WASM sources keep their
# normal __EMSCRIPTEN__ definition.
$(BUILD_DIR)/RandomXManager.o: $(SRC_DIR)/RandomXManager.cpp
	@echo "Compiling RandomXManager in RANDOMX FAST/FULL_MEM mode..."
	@mkdir -p $(BUILD_DIR)/fast
	@sed \
		-e 's/^#ifdef __EMSCRIPTEN__$$/#if defined(__EMSCRIPTEN__) \&\& !defined(RANDOMX_WASM_FAST)/' \
		-e 's/^#ifndef __EMSCRIPTEN__$$/#if !defined(__EMSCRIPTEN__) || defined(RANDOMX_WASM_FAST)/' \
		-e 's/detectedFlags & ~RANDOMX_FLAG_FULL_MEM/detectedFlags \& ~(RANDOMX_FLAG_FULL_MEM | RANDOMX_FLAG_JIT)/' \
		-e 's/flags = detectedFlags |/flags = (detectedFlags \& ~RANDOMX_FLAG_JIT) |/' \
		-e 's/^    flags |= RANDOMX_FLAG_JIT;$$/    \/\/ JIT disabled in WASM Fast mode; use interpreted FULL_MEM VM./' \
		-e 's/^    cacheAllocFlags |= RANDOMX_FLAG_JIT;$$/    \/\/ JIT disabled in WASM Fast mode; use interpreted FULL_MEM cache./' \
		-e 's/^                saveDataset(datasetFileName);$$/                \/\/ Do not duplicate the ~2.08 GiB dataset into MEMFS in the browser./' \
		$(SRC_DIR)/RandomXManager.cpp > $(BUILD_DIR)/fast/RandomXManager.cpp
	$(CXX) $(CXXFLAGS) -DRANDOMX_WASM_FAST $(INCLUDES) -c $(BUILD_DIR)/fast/RandomXManager.cpp -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling optimized WebAssembly Object $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJECTS)
	@echo "Linking optimized WebAssembly Target $(TARGET)..."
	$(CXX) $(OBJECTS) $(RANDOMX_LIB) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete! Output generated successfully in: $(BIN_DIR)/"

clean:
	@echo "Cleaning build artifacts..."
	rm -rf build/*.o build/fast
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
	@echo "RandomX mode: FAST / FULL_MEM (~2.08 GiB dataset)"
	@echo "WASM maximum memory: 3 GiB"

.PHONY: all directories randomx clean distclean rebuild info
