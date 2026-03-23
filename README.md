<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

<div align="center">

![Canopy](image.png)

</div>

# Canopy

[![License](https://img.shields.io/badge/license-All%20Rights%20Reserved-red.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17%2F20-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.24%2B-green.svg)](https://cmake.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)](README.md)

**A Modern C++ Remote Procedure Call Library for High-Performance Distributed Systems**

> Note: Canopy is in Beta, including the documentation, and is under active development.

---

## Why Canopy?

Distributed C++ systems have always been hard. Getting two components talking across a process boundary, a network connection, or a security enclave typically means writing a large amount of hand-rolled serialization, connection management, and error-handling code — code that is fragile, hard to test, and has to be rewritten every time the transport or wire format changes.

Canopy takes the classical RPC model — define an interface, get a proxy on the caller side and a stub on the callee side — and brings it fully up to date with modern C++:

<div align="center">
<pre>
           .idl  
         │
         ┌────┴────┐
       proxy      stub
         │          │
      caller      callee
</pre>
</div>

**Write the interface once in IDL.** Canopy generates type-safe C++ proxy and stub code from a simple Interface Definition Language. You call a remote object exactly as you would a local one; marshalling, routing, and lifecycle management are handled for you.

---

<div align="center">
<pre>
  ┌──────────────────────────────────────┐
   direct  IPC   DLL   TCP   TLS   SGX
  └──────────────────────────────────────┘
       same generated interface
</pre>
</div>

**Works across every boundary you care about.** The same generated code runs over in-process direct calls, shared-memory SPSC queues, TCP sockets, TLS-encrypted streams, and SGX secure enclaves. Switching transport is a matter of changing which stream or transport you construct — your interface code does not change.

---

<div align="center">
<pre>
 ╔═════════════ TLS ════════════════╗
 ║ ╔═══════════ TCP ══════════════╗ ║
 ║ ║ ╔═════════ SPSC ═══════════╗ ║ ║
 ║ ║ ║    streaming::stream     ║ ║ ║
 ║ ║ ╚══════════════════════════╝ ║ ║
 ║ ╚══════════════════════════════╝ ║
 ╚══════════════════════════════════╝
</pre>
</div>

**Streams compose.** Transport streams stack cleanly: wrap a TCP stream in an SPSC buffering layer, then wrap that in TLS, and hand the result to the transport. Each layer only knows about the `stream` interface below it. Adding encryption, compression, or custom framing requires no changes to the RPC layer above or the network layer below.

---

<div align="center">
<pre>
  ┌──── build flag ────┐
  │                    │
  ▼                    ▼
blocking            co_await
 A→B→C→D            A→B→C→D
    (same source code, two modes)
</pre>
</div>

**Blocking and coroutine modes from the same source.** The same C++ implementation compiles in both a straightforward blocking mode (useful for debugging and simple deployments) and a full coroutine mode using C++20 `co_await`. Switching between them is a build flag; your code does not change. This matters particularly for AI-assisted development: LLMs can generate and reason about Canopy interfaces and implementations reliably because there is no hidden async machinery to infer.

---

<div align="center">
<pre>
            ┌──[root zone]──┐
           /        │        \
       [zone A]  [zone B]  [zone C]
         │          │         │
       [sub]    peer link    [sub]
     /   \
         node A    node B
</pre>
</div>

**Distributed by design.** Each machine or process hosts its own root zone. Child zones branch from it for plugins, enclaves, or any other isolation boundary. Multiple nodes connect as peers over the network. Objects living at any depth in any node's zone tree can call objects at any depth in any other node's tree — the routing is automatic.

---

<div align="center">
<pre>
  ╭──────────────────────────────────╮
  │  BINARY ◄────────●────────► JSON │
  │            PROTO   YAS           │
  │         per-connection dial      │
  ╰──────────────────────────────────╯
</pre>
</div>

**Serialization formats is a choice, not a commitment.** Binary YAS format for production throughput, compressed binary for bandwidth-constrained links, JSON for human-readable debugging and cross-language interop, Protocol Buffers for teams that need a language-neutral wire format. The format can be negotiated per-connection or overridden per-call.

---

<div align="center">
<pre>
  caller                      callee
   🐒  ══[post]══▶▶▶▶▶▶▶▶   🐒
   │
   └──▶ continues immediately
           (no reply needed)
</pre>
</div>

**One-directional calls for fire-and-forget workloads and streaming, good for financial data or streaming media.** Methods marked `[post]` are sent without waiting for a reply — the caller continues immediately. This eliminates round-trip latency for workloads where the caller does not need a result: streaming media frames, LLM inference token delivery, telemetry events, log records, or any high-throughput notification pattern.

---

<div align="center">
<pre>
            ┌──[i_foo]──▶ 
  [remote object]──┼──[i_bar]──▶ class X<i_foo, i_bar, i_baz>
            └──[i_baz]──▶ 
     cast performed against live object
</pre>
</div>

**Polymorphism and Multiple Inheritance.** A single remote object can implement multiple interfaces simultaneously, and many different classes can implement the same interface. Callers hold a proxy to one interface and can remotely cast to any other interface the object supports — the cast is performed against the live object in its zone, not a local copy. This gives you the full expressiveness of C++ polymorphism over any transport, without being limited to the single flat contracts that most RPC systems impose.

---

<div align="center">
<pre>
  [zone] ──── discover ────▶  { i_calculator }  ──▶ MCP
    ?                 { i_logger     }
                      { i_storage    }
</pre>
</div>

**Remote reflection.** Canopy carries interface metadata across zone boundaries, making it possible to discover what interfaces a remote object supports at runtime. This opens the door to generic tooling, dynamic proxies, and runtime composition — capabilities that are normally reserved for languages with built-in reflection and are unusual in a C++ RPC system. One practical application is implementing Model Context Protocol (MCP) services: because Canopy can enumerate the methods and types of a remote object at runtime, it can generate MCP tool descriptions dynamically, allowing AI assistants to discover and call C++ services without any hand-written schema.

If you are building a C++ system that needs components to talk to each other — whether on the same machine, across a data centre, or inside a hardware security boundary — Canopy is designed to make that straightforward rather than painful.

---

## Key Features

- **Type-Safe**: Full C++ type system integration with compile-time verification
- **Transport Agnostic**: Local, TCP, SPSC, SGX Enclave, and custom transports
- **Composable Streams**: Stack TCP, TLS, SPSC, WebSocket layers in any combination
- **Format Agnostic**: YAS binary, compressed binary, JSON, Protocol Buffers
- **Bi-Modal Execution**: Same code runs in both blocking and coroutine modes
- **SGX Enclave Support**: Secure computation in Intel SGX enclaves
- **Comprehensive Telemetry**: Sequence diagrams, console output, HTML animations
- **Coroutine Library Agnostic**: libcoro, libunifex, cppcoro, Asio (see [08-coroutine-libraries.md](documents/08-coroutine-libraries.md))
- **AddressSanitizer Support**: Full ASan integration with 972 tests passing (100% memory safety validated)

---

## Documentation

Comprehensive documentation is available in the [documents/](documents/) directory:

### Getting Started
1. [Introduction](documents/01-introduction.md) - What is Canopy and its key features
2. [Getting Started Tutorial](documents/02-getting-started.md) - Step-by-step tutorials
3. [IDL Guide](documents/03-idl-guide.md) - Interface Definition Language syntax and usage
4. [Building Canopy](documents/04-building.md) - Build configuration and CMake presets
5. [Bi-Modal Execution](documents/05-bi-modal-execution.md) - Blocking and coroutine modes
6. [Error Handling](documents/06-error-handling.md) - Error codes and handling patterns
7. [Telemetry](documents/07-telemetry.md) - Debugging and visualization
8. [Coroutine Libraries](documents/08-coroutine-libraries.md) - Coroutine library support and porting
9. [API Reference](documents/09-api-reference.md) - Quick reference for main APIs
10. [Examples](documents/10-examples.md) - Working examples and demos
11. [Best Practices](documents/11-best-practices.md) - Design guidelines and troubleshooting

### Architecture
- [Architecture Overview](documents/architecture/01-overview.md) - Zones, services, transports, memory management
  - Detailed guides: [Local](documents/transports/local.md), [TCP](documents/transports/tcp.md), [SPSC](documents/transports/spsc.md), [SGX](documents/transports/sgx.md), [Custom](documents/transports/custom.md)

### Serialization
- [YAS Serializer](documents/serializers/yas-serializer.md) - Binary, JSON, and compressed formats
- [Protocol Buffers](documents/serializers/protocol-buffers.md) - Cross-language serialization

---

## Quick Start

### Prerequisites

- **C++17 Compiler**: Clang 10+, GCC 9.4+, or Visual Studio 2019+
- **CMake**: 3.24 or higher
- **Build System**: Ninja (recommended)
- **Node.js**: 18+ (for llhttp code generation)
- **OpenSSL**: Development headers (libssl-dev on Linux, OpenSSL SDK on Windows)
- **clang-tidy** (optional): LLVM 16+ for static analysis; LLVM 21+ recommended for full check coverage including `modernize-use-designated-initializers`

### Build

```bash
# Clone and configure
git clone https://github.com/edwardbr/Canopy.git
cd Canopy

# Blocking (synchronous) mode
cmake --preset Debug
cmake --build build_debug

# Coroutine (async/await) mode
cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine

# With AddressSanitizer
cmake --preset Debug_ASAN
cmake --build build_debug

cmake --preset Debug_Coroutine_ASAN
cmake --build build_debug_coroutine

# Coverage builds
cmake --preset Debug_Coverage
cmake --build build_debug

cmake --preset Debug_Coroutine_Coverage
cmake --build build_debug_coroutine

# Static analysis with clang-tidy (requires LLVM 16+)
cmake --preset Debug_Coroutine_Tidy
cmake --build build_debug_coroutine_tidy

# Run tests
ctest --test-dir build_debug --output-on-failure
ctest --test-dir build_debug_coroutine --output-on-failure
```

### Local User Presets

For machine-specific or personal presets, create `CMakeUserPresets.json` from the template:

```bash
cp CMakeUserPresets.json.example CMakeUserPresets.json
cmake --list-presets
```

This keeps your custom presets local while still inheriting from project presets.

### Build Options

```cmake
# Execution mode
CANOPY_BUILD_COROUTINE=ON    # Enable async/await support (requires C++20)

# Features
CANOPY_BUILD_ENCLAVE=ON      # SGX enclave support
CANOPY_BUILD_TEST=ON         # Test suite
CANOPY_BUILD_DEMOS=ON        # Demo applications

# Development
CANOPY_USE_LOGGING=ON        # Comprehensive logging
CANOPY_USE_TELEMETRY=ON      # Debugging and visualization
CANOPY_DEBUG_GEN=ON          # Code generation debugging

# Memory Safety
CANOPY_DEBUG_ADDRESS=ON      # AddressSanitizer (detect memory errors)
CANOPY_DEBUG_THREAD=ON       # ThreadSanitizer (detect data races)
CANOPY_DEBUG_UNDEFINED=ON    # UndefinedBehaviorSanitizer
```

---

## Hello World Example

**calculator.idl:**
```idl
namespace calculator {
    [inline] namespace v1 {
        [status=production]
        interface i_calculator {
            error_code add(int a, int b, [out] int& result);
        };
    }
}
```

**Server** — listen on TCP, wrap each accepted connection in TLS, serve the calculator:
```cpp
#include "generated/calculator/calculator.h"
#include <streaming/listener.h>
#include <streaming/tcp/acceptor.h>
#include <streaming/tls/stream.h>
#include <transports/streaming/transport.h>

using namespace calculator::v1;

auto service = std::make_shared<rpc::root_service>("calc_server", server_zone, scheduler);

auto tls_ctx = std::make_shared<streaming::tls::context>(cert_path, key_path);

// stream_transformer: wrap each raw TCP stream in TLS before handing it to the transport
auto tls_transformer = [tls_ctx, scheduler](std::shared_ptr<streaming::stream> tcp_stm)
    -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
{
    auto tls_stm = std::make_shared<streaming::tls::stream>(tcp_stm, tls_ctx);
    if (!CO_AWAIT tls_stm->handshake())
        CO_RETURN std::nullopt;  // reject connection if handshake fails
    CO_RETURN tls_stm;
};

auto listener = std::make_shared<streaming::listener>("calc_server",
    std::make_shared<streaming::tcp::acceptor>(endpoint),
    rpc::stream_transport::make_connection_callback<i_calculator, i_calculator>(
        [](const rpc::shared_ptr<i_calculator>&,
            const std::shared_ptr<rpc::service>& svc)
            -> CORO_TASK(rpc::service_connect_result<i_calculator>)
        {
            // Welcome you are in RPC land!
            CO_RETURN rpc::service_connect_result<i_calculator>{
                rpc::error::OK(),
                rpc::shared_ptr<i_calculator>(new my_calculator_impl(svc))};
        }),
    std::move(tls_transformer));

listener->start_listening(service);
```

**Client** — connect via TCP, perform TLS handshake, call the remote calculator:
```cpp
#include "generated/calculator/calculator.h"
#include <streaming/tcp/stream.h>
#include <streaming/tls/stream.h>
#include <transports/streaming/transport.h>

using namespace calculator::v1;

auto client_service = std::make_shared<rpc::root_service>("calc_client", client_zone, scheduler);

// 1. Establish TCP connection
coro::net::tcp::client tcp_client(scheduler, endpoint);
CO_AWAIT tcp_client.connect(std::chrono::milliseconds{5000});
auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(tcp_client), scheduler);

// 2. Wrap in TLS
auto tls_ctx = std::make_shared<streaming::tls::client_context>(/*verify_peer=*/true);
auto tls_stm = std::make_shared<streaming::tls::stream>(tcp_stm, tls_ctx);
CO_AWAIT tls_stm->client_handshake();

// 3. Create transport and connect to the remote zone
auto transport = rpc::stream_transport::make_client("calc_client", client_service, tls_stm);

rpc::shared_ptr<i_calculator> input_iface;
auto connect_result = CO_AWAIT client_service->connect_to_zone<i_calculator, i_calculator>(
    "calc_server", transport, input_iface);

if (connect_result.error_code != rpc::error::OK())
{
    // handle connection failure
}
auto calc = connect_result.output_interface;

// 4. Make RPC call
int result;
auto error = CO_AWAIT calc->add(5, 3, result);
std::cout << "5 + 3 = " << result << std::endl;  // Output: 5 + 3 = 8
```

For a complete working example see `demos/stream_composition/src/tcp_spsc_tls_demo.cpp`.

---

## Supported Transports

| Transport | Description | Requirements |
|-----------|-------------|--------------|
| **Local** | In-process parent-child communication | None |
| **TCP** | Network communication between machines | Coroutines |
| **SPSC** | Single-producer single-consumer queues | Coroutines |
| **SGX Enclave** | Secure enclave communication | SGX SDK |
| **Custom** | User-defined transport implementations | Custom implementation |

See [transport documentation](documents/transports/) for details.

---

## Requirements

### Supported Platforms
- **Windows**: Visual Studio 2019+
- **Linux**: Ubuntu 18.04+, CentOS 8+
- **Embedded**: Any platform with C++17 support

### Compilers
- **Clang**: 10.0+ (LLVM 21 recommended for full clang-tidy support)
- **GCC**: 9.4+
- **MSVC**: Visual Studio 2019+

### Dependencies
Git submodules manage external dependencies:
- **YAS**: Serialization framework
- **libcoro**: Coroutine support (when `CANOPY_BUILD_COROUTINE=ON`)
- **range-v3**: Range library support
- **spdlog**: Logging framework

---

## Project Structure

```
canopy/
├── rpc/                    # Core RPC library
├── generator/              # IDL code generator
├── transports/             # Transport implementations (local, tcp, spsc, sgx)
├── tests/                  # Test suite
├── demos/                  # Example applications
├── telemetry/              # Telemetry and logging
├── cmake/                  # CMake build configuration modules
│   ├── Canopy.cmake        # Main build configuration
│   ├── Linux.cmake         # Linux-specific settings
│   ├── Windows.cmake       # Windows-specific settings
│   ├── SGX.cmake           # SGX enclave support
│   └── CanopyGenerate.cmake # IDL code generation
├── documents/              # Comprehensive documentation
├── submodules/             # External dependencies
└── CMakeLists.txt          # Build configuration
```

---

## Development Setup

### Linux Installation (Fedora 43+)

Install system dependencies:

```bash
sudo dnf install gcc gcc-c++ clang clang-tools-extra openssl-devel wget make perl-core zlib-devel ninja-build nodejs gdb python3-pip liburing-devel
pip install --user cmakelang
```

`clang-tools-extra` includes `clang-tidy` and `clang-format`. The Fedora 43 repos ship LLVM 21, which supports all checks used in this project including `modernize-use-designated-initializers`.

Install CMake 4.x or later (the version in the Fedora repos may be too old):

```bash
# Download and install the CMake 4.2.3 prebuilt binary
wget https://github.com/Kitware/CMake/releases/download/v4.2.3/cmake-4.2.3-linux-x86_64.tar.gz
tar -zxf cmake-4.2.3-linux-x86_64.tar.gz
sudo cp -r cmake-4.2.3-linux-x86_64/* /usr/local/
```

### Code Formatting

This project uses `cmake-format` for CMake files and `clang-format` for C++ files (both installed above).

**VSCode Setup:**
1. Open the project in VSCode
2. Install recommended extensions when prompted (or manually install `cheshirekow.cmake-format`)
3. The workspace settings will automatically use `.cmake-format.yaml` for formatting
4. Format-on-save is enabled by default

**Manual formatting:**
```bash
# Check CMake formatting
git ls-files -- \*.cmake \*CMakeLists.txt | xargs cmake-format --check

# Apply CMake formatting
git ls-files -- \*.cmake \*CMakeLists.txt | xargs cmake-format -i

# Apply C++ formatting
clang-format -i <file>
```

---

## Contributing

Canopy is actively maintained.

- Performance optimizations
- New transport implementations
- Platform ports
- Documentation improvements

---

## License

Copyright (c) 2026 Edward Boggis-Rolfe. All rights reserved.

See [LICENSE](LICENSE) for details.

---

## Acknowledgments

**SHA3 Implementation**: Credit to [brainhub/SHA3IUF](https://github.com/brainhub/SHA3IUF)

---

*For technical questions and detailed API documentation, see the [documents directory](documents/).*
