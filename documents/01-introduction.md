<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Introduction to Canopy

Canopy is a modern C++ Remote Procedure Call library that enables type-safe communication across different execution contexts. It provides a unified programming model for in-process calls, inter-process communication, remote machines, embedded devices, and secure Intel SGX enclaves.

## What is Canopy?

Canopy bridges the gap between local C++ objects and distributed systems by providing:

- **Type Safety**: Full C++ type system integration with compile-time verification through IDL-generated code
- **Transport Independence**: A single API works across multiple transport mechanisms
- **Format Flexibility**: Support for JSON, binary (YAS), and Protocol Buffers serialization
- **Bi-Modal Execution**: Same code runs in both blocking and coroutine modes
- **Secure Enclaves**: Native support for Intel SGX secure computation

## Key Features

### Type-Safe Interface Definitions

Define interfaces using a C++-like Interface Definition Language (IDL):

```idl
namespace calculator
{
    interface i_calculator
    {
        error_code add(int a, int b, [out] int& result);
        error_code multiply(int a, int b, [out] int& result);
    };
}
```

The IDL compiler generates:
- Pure virtual interface classes
- Proxy implementations for clients
- Stub implementations for servers
- Serialization code for all parameters
- JSON schema metadata for introspection

### Transport Agnostic Design

Canopy abstracts transport details behind a consistent interface. Multiple transports are available:

| Transport | Use Case | Requirements |
|-----------|----------|--------------|
| [Local](transports/local.md) | In-process parent/child zones | None |
| [TCP](transports/tcp.md) | Network communication | CANOPY_BUILD_COROUTINE=ON |
| [SPSC and IPC](transports/spsc_and_ipc.md) | Lock-free single producer/consumer IPC and process-hosted streaming | CANOPY_BUILD_COROUTINE=ON |
| [SGX](transports/sgx.md) | Secure enclave communication | CANOPY_BUILD_ENCLAVE=ON |
| [Custom](transports/custom.md) | User-defined transports | Varies |

For detailed transport documentation, see the [transports directory](transports/).
For stream-backed transport behaviour under saturation, see
[Stream Backpressure Guidelines](transports/stream_backpressure_guidelines.md).

### Bi-Modal Execution

Write code once, deploy in blocking or coroutine mode:

```cpp
CORO_TASK(error_code) calculate(int a, int b, int& result)
{
    auto error = CO_AWAIT calculator_->add(a, b, result);
    CO_RETURN error;
}
```

- **Blocking Mode**: `CORO_TASK(int)` resolves to `int`, `CO_AWAIT` is no-op
- **Coroutine Mode**: `CORO_TASK(int)` becomes `coro::task<int>`, `CO_AWAIT` suspends execution

For complete details on bi-modal execution, see [Bi-Modal Execution](05-bi-modal-execution.md).

For a deep dive into Canopy's architecture (zones, services, transports, memory management), see [Architecture Overview](architecture/01-overview.md).

## When to Use Canopy

Canopy is ideal for:

1. **Microservices in C++**: Build distributed systems with type safety
2. **Embedded Systems**: Efficient IPC with minimal overhead
3. **Secure Computation**: Intel SGX enclave support for sensitive operations
4. **Cross-Process Communication**: Seamless object passing between processes
5. **Plugin Architectures**: Load/unload plugins with automatic cleanup

## Design Philosophy

Canopy follows several key design principles:

1. **Type Safety First**: Compile-time verification of all RPC calls
2. **No Unsafe Casting**: Never mix `rpc::shared_ptr` with `std::shared_ptr`
3. **RAII Throughout**: Resource management via constructors/destructors
4. **Coroutines First**: API designed for async/await patterns
5. **Transparent Marshalling**: Parameters pass like local objects

## Project Structure

```
canopy/
├── c++/                      # C++ source code
│   ├── rpc/                  # Core RPC library
│   │   ├── include/rpc/      # Public headers
│   │   │   └── internal/     # Internal implementation
│   │   └── src/              # Core library implementation
│   ├── transports/           # Transport implementations
│   │   ├── local/            # In-process transport
│   │   ├── sgx/              # SGX enclave transport
│   │   └── streaming/        # Streaming transport
│   ├── tests/                # Test suite
│   ├── demos/                # Demo applications
│   ├── telemetry/            # Telemetry services
│   ├── streaming/            # Coroutine streaming stack
│   └── subcomponents/        # Network config, SPSC queue, etc.
├── generator/                # IDL code generator
├── interfaces/               # Shared IDL interfaces (rpc_types.idl, etc.)
└── submodules/               # Core dependencies (idlparser, protobuf)
```

## Version and Requirements

- **C++ Standard**: C++17 (C++20 when CANOPY_BUILD_COROUTINE=ON)
- **CMake**: 3.24+
- **Compilers**: Clang 10+, GCC 9.4+, MSVC 2019+

## Next Steps

1. [Getting Started](02-getting-started.md) - Follow a tutorial
2. [IDL Guide](03-idl-guide.md) - Learn to define interfaces
3. [Building Canopy](04-building.md) - Set up your build environment
4. [Bi-Modal Execution](05-bi-modal-execution.md) - Understand blocking vs coroutine modes
5. [Error Handling](06-error-handling.md) - Handle errors in RPC calls

**Advanced Reading:**
- [Architecture Overview](architecture/01-overview.md) - Internal plumbing and advanced features
- [Zone Hierarchies](architecture/07-zone-hierarchies.md) - Multi-level zone topologies
- [Transports and Passthroughs](architecture/06-transports-and-passthroughs.md) - Communication layer details
