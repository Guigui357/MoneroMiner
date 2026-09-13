cmake_minimum_required(VERSION 3.15)

project(RandomX LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Common sources
# ============================================================

set(randomx_sources
    src/aes_hash.cpp
    src/argon2_ref.c
    src/argon2_core.c

    src/bytecode_machine.cpp
    src/cpu.cpp
    src/dataset.cpp
    src/soft_aes.cpp
    src/virtual_memory.c

    src/vm_interpreted.cpp
    src/vm_interpreted_light.cpp

    src/allocator.cpp
    src/instruction.cpp
    src/randomx.cpp
    src/superscalar.cpp
    src/virtual_machine.cpp

    src/blake2_generator.cpp
    src/instructions_portable.cpp
    src/reciprocal.c
    src/blake2/blake2b.c
)

# ============================================================
# WebAssembly / Emscripten
# ============================================================

if(EMSCRIPTEN)
    # Stage 1 WASM JIT emitter. It is deliberately separate from the
    # RandomX VM for now; the next stage will connect it to program
    # generation/execution after canonical vectors are validated.
    list(APPEND randomx_sources
        src/wasm_jit.cpp
    )
endif()

add_library(randomx STATIC
    ${randomx_sources}
)

target_include_directories(randomx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(EMSCRIPTEN)
    message(STATUS "=========================================")
    message(STATUS "  RandomX WebAssembly build")
    message(STATUS "=========================================")
    message(STATUS "Native RandomX JIT: DISABLED")
    message(STATUS "Backend: interpreter + WASM JIT stage 1")

    if(RANDOMX_WASM_SIMD)
        target_compile_options(randomx PRIVATE -msimd128)
        message(STATUS "WebAssembly SIMD128: ENABLED")
    else()
        message(STATUS "WebAssembly SIMD128: DISABLED")
    endif()

    if(RANDOMX_WASM_PTHREADS)
        target_compile_options(randomx PRIVATE -pthread)
        target_link_options(randomx INTERFACE -pthread)
        message(STATUS "WebAssembly pthreads: ENABLED")
    else()
        message(STATUS "WebAssembly pthreads: DISABLED")
    endif()

    target_compile_definitions(randomx PRIVATE
        RANDOMX_WASM=1
    )
endif()

# ============================================================
# Native optimizations
# ============================================================

if(NOT EMSCRIPTEN)

    if(
        CMAKE_SYSTEM_PROCESSOR MATCHES
        "x86_64|AMD64|amd64"
    )

        include(CheckCCompilerFlag)

        check_c_compiler_flag(
            "-maes"
            HAVE_AES
        )

        if(HAVE_AES)

            target_compile_options(
                randomx
                PRIVATE
                    -maes
            )

        endif()

        check_c_compiler_flag(
            "-mssse3"
            HAVE_SSSE3
        )

        if(HAVE_SSSE3)

            set_source_files_properties(
                src/argon2_ssse3.c
                PROPERTIES
                COMPILE_OPTIONS "-mssse3"
            )

            list(APPEND randomx_sources
                src/argon2_ssse3.c
            )

        endif()

        check_c_compiler_flag(
            "-mavx2"
            HAVE_AVX2
        )

        if(HAVE_AVX2)

            set_source_files_properties(
                src/argon2_avx2.c
                PROPERTIES
                COMPILE_OPTIONS "-mavx2"
            )

            list(APPEND randomx_sources
                src/argon2_avx2.c
            )

        endif()

    endif()

endif()

# ============================================================
# Emscripten target settings
# ============================================================

if(EMSCRIPTEN)

    target_compile_options(
        randomx
        PRIVATE
            -O3
    )

    if(RANDOMX_WASM_SIMD)

        target_compile_options(
            randomx
            PRIVATE
                -msimd128
        )

    endif()

    if(RANDOMX_WASM_PTHREADS)

        target_compile_options(
            randomx
            PRIVATE
                -pthread
        )

    endif()

endif()

# ============================================================
# Tests / benchmark
# ============================================================

if(NOT EMSCRIPTEN)

    add_executable(
        randomx-tests
        src/tests/tests.cpp
    )

    target_link_libraries(
        randomx-tests
        PRIVATE randomx
    )

    add_executable(
        randomx-codegen
        src/tests/code-generator.cpp
    )

    target_link_libraries(
        randomx-codegen
        PRIVATE randomx
    )

    add_executable(
        randomx-benchmark
        src/tests/benchmark.cpp
        src/tests/affinity.cpp
    )

    find_package(Threads REQUIRED)

    target_link_libraries(
        randomx-benchmark
        PRIVATE
            randomx
            Threads::Threads
    )

endif()
