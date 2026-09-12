# Miner V2 performance audit

This branch is intended to optimize the browser/WebAssembly mining hot path without changing the RandomX algorithm or share validation semantics.

## Findings

1. **RandomX is already running with WASM PThreads and SIMD.** The Makefile enables `USE_PTHREADS`, LTO, and `-msimd128`.
2. **WASM deliberately uses RandomX LIGHT mode.** `RandomXManager` disables FULL_MEM, dataset creation, large pages, and JIT under `__EMSCRIPTEN__`. This is a deliberate compatibility choice and should not be changed blindly.
3. **The mining loop performs avoidable work for every nonce.** In `MoneroMiner.cpp`, each hash currently:
   - copies the job blob with `jobCopy.getBlobBytes()`;
   - allocates a new 32-byte `std::vector<uint8_t>` for the target;
   - converts the 4 target words into that vector;
   - clears the 32-byte hash result even though `randomx_calculate_hash()` overwrites the output;
   - performs additional debug formatting when debug mode is enabled.
4. **Hash counting already exists.** `MiningThreadData` increments its hash counter and the browser hashrate is already exposed, so a V2 optimization must not add a second hashrate subsystem.
5. **Each mining thread owns a RandomX VM.** `RandomXManager` stores VMs by thread ID, which is the correct direction for avoiding VM contention.

## V2 optimization target

Move all job-invariant allocations/conversions out of the per-nonce loop. The hot loop should only:

1. write the 32-bit nonce into the already allocated blob;
2. call RandomX;
3. compare the 32-byte result against the already prepared target;
4. submit only when valid.

The next implementation should preserve the existing nonce partitioning, pool protocol, target byte order, and share submission behavior.

## Thread pool note

The build currently reserves six Emscripten pthread workers. The JavaScript side chooses the mining thread count from browser CPU information, so the pool size should be revisited together with the final thread-selection policy rather than changed independently.
