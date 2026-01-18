<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

![Canopy](image.png)

# Canopy

[![License](https://img.shields.io/badge/license-All%20Rights%20Reserved-red.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17%2F20-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.24%2B-green.svg)](https://cmake.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)](README.md)

**A Modern C++ Remote Procedure Call Library for High-Performance Distributed Systems**

Canopy enables type-safe communication across execution contexts including in-process calls, inter-process communication, remote machines, embedded devices, and secure enclaves (SGX).

Note this is in Beta and is in active development

---

## Key Features

- **🔒 Type-Safe**: Full C++ type system integration with compile-time verification
- **🌐 Transport Agnostic**: Local, TCP, SPSC, SGX Enclave, and custom transports
- **📦 Format Agnostic**: JSON, YAS binary, Protocol Buffers support
- **🔄 Bi-Modal Execution**: Same code runs in both blocking and coroutine modes
- **🛡️ SGX Enclave Support**: Secure computation in Intel SGX enclaves
- **📊 Comprehensive Telemetry**: Sequence diagrams, console output, HTML animations
- **🔧 Coroutine Library Agnostic**: libcoro, libunifex, cppcoro, Asio (see [13-coroutine-libraries.md](documents/13-coroutine-libraries.md))

---

## Documentation

Comprehensive documentation is available in the [documents/](documents/) directory:

### Getting Started
1. [Introduction](documents/01-introduction.md) - What is Canopy and its key features
2. [Core Concepts](documents/02-core-concepts.md) - Zones, services, smart pointers, proxies, and stubs
3. [IDL Guide](documents/03-idl-guide.md) - Interface Definition Language syntax and usage
4. [Transports](documents/04-transports.md) - Local, TCP, SPSC, and SGX enclave transports
   - Detailed guides: [Local](documents/transports/local.md), [TCP](documents/transports/tcp.md), [SPSC](documents/transports/spsc.md), [SGX](documents/transports/sgx.md), [Custom](documents/transports/custom.md)
5. [Building Canopy](documents/05-building.md) - Build configuration and CMake presets
6. [Getting Started Tutorial](documents/06-getting-started.md) - Step-by-step tutorials

### Advanced Topics
7. [Bi-Modal Execution](documents/07-bi-modal-execution.md) - Blocking and coroutine modes
8. [Error Handling](documents/08-error-handling.md) - Error codes and handling patterns
9. [Telemetry](documents/09-telemetry.md) - Debugging and visualization
10. [Memory Management](documents/10-memory-management.md) - Reference counting and lifecycle
11. [Zone Hierarchies](documents/11-zone-hierarchies.md) - Complex distributed topologies
12. [API Reference](documents/12-api-reference.md) - Quick reference for main APIs
13. [Coroutine Libraries](documents/13-coroutine-libraries.md) - Coroutine library support and porting
14. [Examples](documents/14-examples.md) - Working examples and demos
15. [Best Practices](documents/15-best-practices.md) - Design guidelines and troubleshooting
16. [Migration Guide](documents/16-migration-guide.md) - Upgrading from older versions

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

### Build

```bash
# Clone and configure
git clone https://github.com/edwardbr/Canopy.git
cd Canopy

# Choose Blocking 
cmake --preset Debug

# Or Coroutines
cmake --preset Coroutine_Debug

# Build core library
cmake --build build_debug --target rpc

# Run tests
ctest --test-dir build_debug --output-on-failure
```

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
CANOPY_DEBUG_GEN=ON            # Code generation debugging
```

---

## Hello World Example

**calculator.idl:**
```idl
namespace calculator {
    [inline] namespace v1 {
        [status=production]
        interface i_calculator {
            int add(int a, int b, [out] int& result);
        };
    }
}
```

**Usage:**
```cpp
#include "generated/calculator/calculator.h"

// Create service and connect to calculator zone
auto root_service = std::make_shared<rpc::service>("root", rpc::zone{1});
rpc::shared_ptr<calculator::v1::i_calculator> calc;

auto error = CO_AWAIT root_service->connect_to_zone<...>(
    "calc_zone", rpc::zone{2}, nullptr, calc, setup_callback);

// Make RPC call
int result;
error = CO_AWAIT calc->add(5, 3, result);
std::cout << "5 + 3 = " << result << std::endl;  // Output: 5 + 3 = 8
```

---

## Supported Transports

| Transport | Description | Requirements |
|-----------|-------------|--------------|
| **Local** | In-process parent-child communication | None |
| **TCP** | Network communication between machines | Coroutines |
| **SPSC** | Single-producer single-consumer queues | Coroutines |
| **SGX Enclave** | Secure enclave communication | SGX SDK |
| **Custom** | User-defined transport implementations | Custom implementation |

See [04-transports.md](documents/04-transports.md) for details.

---

## Requirements

### Supported Platforms
- **Windows**: Visual Studio 2019+
- **Linux**: Ubuntu 18.04+, CentOS 8+
- **Embedded**: Any platform with C++17 support

### Compilers
- **Clang**: 10.0+
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
