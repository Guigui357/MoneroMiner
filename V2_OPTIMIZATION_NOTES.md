# Miner V2 optimization notes

The V2 hot-path changes remove the pre-hash zeroing of the RandomX output buffer and add a target API that accepts the Job's existing four 64-bit target words, avoiding construction of a second serialized target inside the hashing routine.

The compatibility overload remains available for existing callers.

The mining loop still creates its `targetBytes` vector before calling the compatibility overload. A follow-up change should update `MoneroMiner.cpp` to call the new array overload directly; that requires changing the existing mining-loop call site and should be benchmarked together with the blob-copy elimination.

No automatic/hidden mining behavior is introduced by this branch.
