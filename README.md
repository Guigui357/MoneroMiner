# MoneroMiner v2.0.0-WASM

**Author:** [Hacker Fantastic](https://hacker.house)  
**License:** [BSD-3-Clause](https://opensource.org/license/bsd-3-clause/)
**Documentation:** [Complete API & Implementation Docs](https://deepwiki.com/hackerhouse-opensource/MoneroMiner)

---

## Features

**Core Capabilities:**

- Multi-threaded mining with per-thread nonce ranges
- Full stratum protocol with automatic reconnection
- Real-time hashrate monitoring and share tracking
- Debug mode with detailed hash comparisons

**Advanced:**

- Background job management thread
- Huge pages support (10-30% hashrate boost)
- Share deduplication and stale detection
- Graceful shutdown and cleanup
- No Proxy required for connecting in the pool

---

## Platform Support

**Operating Systems:**

———————————————————————————————-
- **ALL systems with a browser**
———————————————————————————————-
## Quick Start

- access miner-henna-one.vercel.app
- click in Iniciar Mineração

### Architecture

**Core Components:**

- **PoolClient**: Stratum protocol (JSON-RPC 2.0)
- **RandomXManager**: VM/dataset management
- **MiningThreadData**: Per-thread state and statistics
- **Job**: Work encapsulation (blob, target, difficulty)
- **Platform**: OS abstraction (sockets, CPU info, huge pages)

### Nonce Distribution

Each thread mines a unique 32-bit range:

```
Thread 0:  0x00000000 - 0x15555554
Thread 1:  0x15555555 - 0x2AAAAAAA
Thread 2:  0x2AAAAAAB - 0x3FFFFFFF
...
Thread 11: 0xEAAAAAAB - 0xFFFFFFFF
```

Nonces are 4-byte little-endian at blob offset 39-42.

## Donations

If you find MoneroMiner useful, XMR donations support continued development:

```
8C6hFb4Buo6dYwJiZEaFhyYhZTJaR4NyXSBzKMF1BnNKMGD92yeaY3a9PxuWp9bhTAh6dAXwqyyLfFxaPRct7j81L8t4iK2

(This goes to those who originally created this repository)
```

---

## License

These files are available under the 3-clause BSD license.

**Third-Party:**

- RandomX: BSD 3-Clause (see RandomX/LICENSE)
- picojson: BSD 2-Clause
