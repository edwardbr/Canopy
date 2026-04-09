<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Introduction to Canopy

Scope note:

- this introduction is written from the perspective of the primary C++
  implementation
- unless explicitly stated otherwise, references to transports, execution
  modes, serializers, and examples should be read as C++ capabilities
- see [C++ Status](status/cpp.md), [Rust Status](status/rust.md), and
  [JavaScript Status](status/javascript.md) for implementation scope

Who this is for:

- readers deciding whether Canopy fits their problem
- C++ developers who want a high-level view before reading build or API docs
- readers who need a quick overview of current implementation scope

Canopy is a modern C++ Remote Procedure Call library that enables type-safe communication across different execution contexts. It provides a unified programming model for in-process calls, inter-process communication, remote machines, embedded devices, and secure Intel SGX enclaves.

## What is Canopy?

Canopy bridges the gap between local C++ objects and distributed systems by providing:

- **Type Safety**: Full C++ type system integration with compile-time verification through IDL-generated code
- **Transport Independence**: A single API works across multiple transport mechanisms
- **Format Flexibility**: Support for JSON, binary (YAS), and Protocol Buffers serialization
- **Bi-Modal Execution**: Same code runs in both blocking and coroutine modes
- **Secure Enclaves**: Native support for Intel SGX secure computation

Implementation note:

- the C++ implementation is the primary implementation described by this
  document
- the experimental Rust implementation currently supports a smaller subset
  (blocking, Protocol Buffers, local, dynamic-library)
- the JavaScript implementation is currently a reduced-trust generated client
  layer rather than a full runtime equivalent

## How To Think About Canopy

Canopy is best understood as generated RPC for C++ systems that cross
boundaries frequently.

If your application moves between:

- local calls
- child processes
- plugins or dynamic libraries
- network transports
- trust boundaries such as SGX-oriented deployments

then the expensive part is usually not the business logic. It is the repeated
boundary glue: message schemas, serializers, transport plumbing, callback
wiring, and lifetime handling. Canopy exists to turn that repeated glue into
generated code and reusable runtime structure.

## Comparison Points

Compared with common alternatives, Canopy is usually strongest in these cases:

- versus hand-written RPC glue:
  Canopy is a better fit when you want repeatable generated bindings instead of
  bespoke marshalling and transport code for each interface

- versus request/response-only RPC systems:
  Canopy is a better fit when you want remote object references, callbacks, and
  distributed lifetime semantics rather than only stateless message exchange

- versus one-transport plugin or IPC layers:
  Canopy is a better fit when you want to preserve the same interface model
  across local, process, network, and trust boundaries

## When Not To Use Canopy

Canopy is probably not the right choice when:

- your application is entirely local and does not cross meaningful boundaries
- simple HTTP or JSON request/response APIs are sufficient
- you do not want generated code in the build
- your team needs full cross-language runtime parity today rather than a
  C++-primary implementation with smaller Rust and JavaScript support
- you only need a very small amount of one-off IPC and the abstraction cost
  would outweigh the repeated glue you are trying to remove

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

Canopy abstracts transport details behind a consistent interface. In the
primary C++ implementation, multiple transports are available:

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

Canopy follows several key design principles in the primary C++ implementation:

1. **Type Safety First**: Compile-time verification of all RPC calls
2. **No Unsafe Casting**: Never mix `rpc::shared_ptr` with `std::shared_ptr`
3. **RAII Throughout**: Resource management via constructors/destructors
4. **Bi-Modal Runtime Design**: C++ APIs are designed to support both blocking
   and coroutine builds from the same source
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
├── rust/                     # Experimental Rust implementation and migration docs
├── generator/                # IDL code generator
├── interfaces/               # Shared IDL interfaces (rpc_types.idl, etc.)
├── c_abi/                    # Language-neutral ABI specifications
├── documents/                # Product, architecture, and status documentation
└── submodules/               # Core dependencies (idlparser, protobuf)
```

## Version and Requirements

- **C++ Standard**: C++17 (C++20 when CANOPY_BUILD_COROUTINE=ON)
- **CMake**: 3.24+
- **Compilers**: Clang 10+, GCC 9.4+, MSVC 2019+

## Next Steps

1. [Getting Started](02-getting-started.md) - Follow a tutorial
2. [IDL Guide](03-idl-guide.md) - Learn to define interfaces
3. [C++ Build And Test Guide](build-and-test/cpp.md) - Set up your build environment
4. [Bi-Modal Execution](05-bi-modal-execution.md) - Understand blocking vs coroutine modes
5. [Error Handling](06-error-handling.md) - Handle errors in RPC calls

Recommended first hands-on path:

- start with [External Project Guide](external-project-guide.md)
- then try the example consumer app linked from the root [README](../README.md)

**Advanced Reading:**
- [Architecture Overview](architecture/01-overview.md) - Internal plumbing and advanced features
- [Zone Hierarchies](architecture/07-zone-hierarchies.md) - Multi-level zone topologies
- [Transports and Passthroughs](architecture/06-transports-and-passthroughs.md) - Communication layer details
