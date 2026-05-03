<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Protocol Buffers

Canopy supports Protocol Buffers-compatible serialization for cross-language interoperability and standardized binary encoding.

There are two C++ protobuf-compatible backends:

- `protocol_buffers`: the full Google C++ protobuf runtime and generated C++ message API.
- `nanopb`: a small C runtime with Canopy-generated C++ adapters over Nanopb-generated C structs.

Both paths are intended to encode the same `.proto` schema and wire format. The important distinction is runtime footprint and deployment boundary: full protobuf is useful for ordinary host processes that want the Google C++ API and tooling, while Nanopb is the small-runtime path for environments where linking the full protobuf library is not practical.

## 1. Overview

Protocol Buffers (protobuf) is a binary serialization format developed by Google. Canopy integrates with protobuf to enable communication with services written in other languages.

| Feature | Description |
|---------|-------------|
| Format | Binary |
| Cross-language | Yes |
| Schema | Required (.proto files) |
| Performance | Good (slightly slower than YAS binary) |

## 2. Enabling Protocol Buffers

### CMake Configuration

The full Google C++ protobuf backend is controlled by:

```cmake
set(CANOPY_BUILD_PROTOCOL_BUFFERS ON)
```

The Nanopb backend is controlled separately:

```cmake
set(CANOPY_BUILD_NANOPB ON)
```

`CANOPY_BUILD_PROTOCOL_BUFFERS` and `CANOPY_BUILD_NANOPB` are independent.  They
can both be enabled, either one can be enabled alone, or both can be disabled if
the target uses only non-protobuf encodings.

`CanopyGenerate(... protocol_buffers ...)` means "this IDL target requests
protobuf-compatible schema/wire support".  CMake then decides which generated
backends are actually produced:

- if `CANOPY_BUILD_PROTOCOL_BUFFERS=ON`, Canopy generates the full Google C++
  protobuf backend
- if `CANOPY_BUILD_NANOPB=ON`, Canopy generates the Nanopb backend
- if both are ON, both generated backends can coexist and each encoding uses its
  matching backend
- if full protobuf is OFF but Nanopb is ON, Nanopb is the protobuf-compatible
  stand-in for that IDL target and `rpc::encoding::protocol_buffers` is routed
  through Nanopb
- if Nanopb is OFF but full protobuf is ON, `rpc::encoding::nanopb` is routed
  through the full protobuf backend
- if both protobuf-compatible backends are OFF and no other requested encoding
  can satisfy the target, configuration should fail rather than silently linking
  an unwanted runtime

The raw generator flags remain literal: `--protobuf` requests full protobuf
generation and `--nanopb` requests Nanopb generation.  The policy that maps an
IDL-level `protocol_buffers` request onto one or both backends belongs in
`CanopyGenerate.cmake`, not in the generator executable.

```cmake
# In CMakeLists.txt
CanopyGenerate(
    my_interface
    idl/my_interface.idl
    ${SOURCE_DIR}
    ${BUILD_DIR}/generated
    ""
    yas_binary
    yas_json
    protocol_buffers  # Request protobuf-compatible schema/wire support
)
```

### Build Requirements

Protobuf-compatible generation needs `protoc` at build time. Canopy can build it from the protobuf submodule when needed. The full Google C++ runtime is only required at runtime when `CANOPY_BUILD_PROTOCOL_BUFFERS=ON`; Nanopb builds still need protobuf tooling to compile `.proto` files, but do not require the full protobuf runtime in the generated target.

```bash
# Install protobuf
# On Ubuntu/Debian:
sudo apt install libprotobuf-dev protobuf-compiler

# On Fedora:
sudo dnf install protobuf-devel protobuf-compiler
```

### Shared Objects and Protobuf Shutdown

The full Google C++ protobuf runtime is not entirely stateless.  Generated
`*.pb.cc` files and the protobuf library keep module/process static state such
as default instances and registered cleanup callbacks.

When a DLL or shared object statically links the full protobuf runtime and may be
unloaded with `dlclose` / `FreeLibrary`, it must call
`google::protobuf::ShutdownProtobufLibrary()` after all protobuf use has ended
and before the module is unloaded.

Canopy's dynamic-library helper libraries provide this cleanup point:

- non-coroutine C++ DLLs export the required `canopy_dll_shutdown`
- non-coroutine language-neutral C ABI DLLs export the required
  `canopy_dll_shutdown`
- coroutine DLLs run this cleanup before `canopy_libcoro_dll_scheduled_dll_start` returns
- coroutine SPSC DLL runtimes run the same cleanup during runtime stop

These hooks call `ShutdownProtobufLibrary()` only when compiled with
`CANOPY_BUILD_PROTOCOL_BUFFERS`.  Nanopb does not use the Google C++ protobuf
runtime and does not need this shutdown call.

## 3. C++ to Protocol Buffers Type Mapping

The IDL generator automatically maps C++ types to Protocol Buffers types.

### Scalar Types

| C++ Type | Protobuf Type | Notes |
|----------|---------------|-------|
| `int`, `int32_t`, `signed int` | `int32` | |
| `int64_t`, `long`, `long long` | `int64` | `long` treated as 64-bit for portability |
| `uint32_t`, `unsigned int` | `uint32` | |
| `uint64_t`, `unsigned long`, `unsigned long long` | `uint64` | |
| `int16_t`, `short` | `int32` | Protobuf has no 16-bit type |
| `uint16_t`, `unsigned short` | `uint32` | Protobuf has no 16-bit type |
| `int8_t`, `signed char` | `int32` | Protobuf has no 8-bit type |
| `uint8_t`, `unsigned char` | `uint32` | Protobuf has no 8-bit type |
| `float` | `float` | |
| `double`, `long double` | `double` | |
| `bool` | `bool` | |
| `char`, `wchar_t`, `char16_t`, `char32_t` | `int32` | Character as integer |
| `std::string` | `string` | |
| `size_t`, `uintptr_t` | `uint64` | Platform-specific types |
| `ptrdiff_t`, `ssize_t`, `intptr_t` | `int64` | Platform-specific types |
| `error_code` | `int32` | Common typedef |

### Container Types

| C++ Type | Protobuf Type | Example |
|----------|---------------|---------|
| `std::vector<T>` | `repeated T` | `std::vector<int>` -> `repeated int32` |
| `std::array<T, N>` | `repeated T` | `std::array<int, 5>` -> `repeated int32` |
| `std::map<K, V>` | `map<K, V>` | `std::map<string, int>` -> `map<string, int32>` |
| `std::unordered_map<K, V>` | `map<K, V>` | Same mapping as `std::map` |
| `std::flat_map<K, V>` | `map<K, V>` | Same mapping as `std::map` |

### Special Cases

| C++ Type | Protobuf Type | Notes |
|----------|---------------|-------|
| `std::vector<uint8_t>` | `bytes` | Binary data, not repeated uint32 |
| `std::vector<char>` | `bytes` | Binary data |
| `std::vector<signed char>` | `bytes` | Binary data |
| `rpc::shared_ptr<T>` | `rpc.interface_descriptor` | Interface reference |
| `rpc::optimistic_ptr<T>` | `rpc.interface_descriptor` | Interface reference |
| Pointer types (`T*`) | `uint64` | Address only |

## 4. Nested Templates

The generator correctly handles nested templates:

```idl
struct complex_data
{
    std::vector<my_struct> items;           // repeated my_struct
    std::map<std::string, my_struct> lookup; // map<string, my_struct>
    std::map<int, std::vector<int>> nested;  // map<int32, repeated int32> - not directly supported
};
```

**Note**: Protobuf does not support `map<K, repeated V>` directly. For deeply nested types, consider using a wrapper message.

## 5. Generated Protobuf Files

The generator creates `.proto` files automatically:

```protobuf
syntax = "proto3";

package my_project;

message i_calculator_add_Request {
    double first_val = 1;
    double second_val = 2;
}

message i_calculator_add_Response {
    double response = 1;
    int32 result = 2;
}
```

## 6. Using Protobuf in C++

### Serialization

```cpp
#include <my_project/my_project.pb.h>

// Serialize with protobuf
my_project::i_calculator_add_Request request;
request.set_firstval(10.0);
request.set_secondval(20.0);

std::vector<uint8_t> buffer(request.ByteSizeLong());
request.SerializeToArray(buffer.data(), buffer.size());
```

### Deserialization

```cpp
// Deserialize
my_project::i_calculator_add_Request received;
received.ParseFromArray(data.data(), data.size());

double first = received.firstval();
double second = received.secondval();
```

### With Canopy Transport

```cpp
// Send using protobuf encoding
auto error = CO_AWAIT proxy_->send(
    protocol_version,
    rpc::encoding::protocol_buffers,  // Use protobuf
    tag,
    interface_id,
    method_id,
    input_buffer,
    output_buffer);
```

## 7. Performance Comparison

### Size Comparison (Typical)

```
yas_binary:       100 bytes (baseline)
protocol_buffers: 110 bytes (with overhead)
yas_json:         200 bytes (text)
```

### Speed Comparison (Typical)

```
yas_binary:       0.1 ms (fastest)
protocol_buffers: 0.2 ms
yas_json:         1.0 ms (slowest)
```

## 8. When to Use Protocol Buffers

### Use Full Protocol Buffers When

- Communicating with non-C++ services
- Schema evolution is important
- Using established protobuf tooling
- Interoperating with gRPC services
- Running in a normal host process where linking the Google C++ protobuf runtime is acceptable

### Use Nanopb When

- You need protobuf-compatible wire bytes but cannot link the full protobuf runtime
- Runtime footprint and dependency surface matter more than access to the Google C++ generated message API
- You want one `.proto` compatibility contract across host, JavaScript, and constrained-runtime paths

### Use YAS Binary When

- Pure C++ RPC calls
- Maximum performance required
- Schema is stable
- No cross-language requirements

## 9. Error Handling

```cpp
auto error = rpc::deserialise(rpc::encoding::protocol_buffers, data, obj);

if (error == rpc::error::INCOMPATIBLE_SERIALISATION())
{
    // Protobuf not supported or malformed data
    // Fall back to yas_json
}
```

## 10. Best Practices

1. **Document your .proto files** for cross-team use
2. **Version your schemas** carefully
3. **Use reserved fields** when removing fields
4. **Test with other languages** if interop is required
5. **Consider yas_binary** for pure C++ internal calls

## 11. Next Steps

- [Nanopb](nanopb.md) - Current small-runtime protobuf-compatible backend
- [YAS Serializer](yas-serializer.md) - Native C++ serialization
- [Error Handling](../08-error-handling.md) - Error code reference
- [API Reference](../12-api-reference.md) - Complete API
