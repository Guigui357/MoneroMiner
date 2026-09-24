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

    message(STATUS "==========================================")
    message(STATUS " RandomX WebAssembly build")
    message(STATUS "==========================================")

    set(RANDOMX_WASM ON)

    # --------------------------------------------------------
    # IMPORTANT:
    # RandomX native JIT is NOT supported in this WASM build.
    #
    # We use the interpreted VM and the project's own
    # WebAssembly runtime/JIT externally.
    # --------------------------------------------------------

    add_compile_definitions(
        RANDOMX_HAVE_COMPILER=0
    )

    message(STATUS "RandomX native JIT: DISABLED")
    message(STATUS "RandomX VM: interpreted")

    option(
        RANDOMX_WASM_SIMD
        "Enable WebAssembly SIMD128"
        ON
    )

    option(
        RANDOMX_WASM_PTHREADS
        "Enable WebAssembly pthreads"
        ON
    )

    # --------------------------------------------------------
    # Argon2
    #
    # The portable/reference implementation is used on WASM.
    # Do NOT compile x86 AVX2/SSSE3 implementations.
    # --------------------------------------------------------

    message(STATUS "Argon2 backend: reference")

    # --------------------------------------------------------
    # SIMD128
    # --------------------------------------------------------

    if(RANDOMX_WASM_SIMD)

        message(STATUS "WebAssembly SIMD128: ENABLED")

        add_compile_options(
            -msimd128
        )

    else()

        message(STATUS "WebAssembly SIMD128: DISABLED")

    endif()

    # --------------------------------------------------------
    # pthreads
    # --------------------------------------------------------

    if(RANDOMX_WASM_PTHREADS)

        message(STATUS "WebAssembly pthreads: ENABLED")

        add_compile_options(
            -pthread
        )

    else()

        message(STATUS "WebAssembly pthreads: DISABLED")

    endif()

else()

    # ========================================================
    # Native RandomX
    # ========================================================

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)

        if(
            CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
            OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64"
            OR CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64"
        )

            list(APPEND randomx_sources

                src/assembly_generator_x86.cpp
                src/jit_compiler_x86.cpp
                src/jit_compiler_x86_static.S

                src/argon2_ssse3.c
                src/argon2_avx2.c

                src/vm_compiled.cpp
                src/vm_compiled_light.cpp
            )

        endif()

    endif()

endif()

# ============================================================
# Library
# ============================================================

add_library(
    randomx
    STATIC
    ${randomx_sources}
)

target_include_directories(
    randomx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_property(
    TARGET randomx
    PROPERTY POSITION_INDEPENDENT_CODE ON
)

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
        PRIVATE
            randomx
    )

    add_executable(
        randomx-codegen
        src/tests/code-generator.cpp
    )

    target_link_libraries(
        randomx-codegen
        PRIVATE
            randomx
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
