<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# YAS Serializer

Canopy uses YAS (Yet Another Serialization) as its primary serialization framework, supporting binary, compressed binary, and JSON formats.

## 1. Supported YAS Formats

| Format | Type | Use Case |
|--------|------|----------|
| `yas_binary` | Binary | High-performance, small payloads |
| `yas_compressed_binary` | Binary + compression | Large payloads, network transfer |
| `yas_json` | JSON text | Debugging, interoperability |

### Default Format Selection

```cmake
CanopyGenerate(
    my_interface
    idl/my_interface.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json)
```

Runtime connections use the owning service's default encoding. Configure the
process default with `CANOPY_DEFAULT_ENCODING`, or set it per service with
`service->set_default_encoding(...)` before creating new service proxies.

## 2. Using YAS Serialization

### Basic Serialization

```cpp
#include <rpc/internal/serialiser.h>

// Serialize an object
my_struct obj{42, "hello"};
auto serialized = rpc::serialise<std::vector<char>>(obj, rpc::encoding::yas_binary);

// Deserialize
my_struct deserialized;
auto error = rpc::deserialise(rpc::encoding::yas_binary, serialized, deserialized);
if (!error.empty())
{
    // Handle malformed or incompatible data.
}
```

### Getting Serialized Size

```cpp
auto size = rpc::get_saved_size(obj, rpc::encoding::yas_binary);
```

## 3. Format Negotiation

### Generated Proxy Fallback

Generated proxies try the current service-proxy encoding. If the generated
stub/proxy path returns `rpc::error::INCOMPATIBLE_SERIALISATION()` and the
current encoding is not already `yas_json`, generated code changes that
service proxy to `yas_json` and retries the call.

That is generated-proxy behavior, not a generic transport negotiation protocol.
The low-level marshaller and transport APIs use the explicit
`rpc::encoding` carried in their parameter structs.

```cpp
root_service->set_default_encoding(rpc::encoding::yas_binary);
auto error = CO_AWAIT proxy->my_method(input, output);
// If the peer reports incompatible serialization, generated proxy code may
// retry this call with rpc::encoding::yas_json.
```

### Manual Selection

```cpp
root_service->set_default_encoding(rpc::encoding::yas_json);
```

## 4. IDL Type Mapping

### Basic Types

| IDL Type | Serialized As |
|----------|---------------|
| `int`, `int32_t` | 32-bit integer |
| `int64_t` | 64-bit integer |
| `uint32_t`, `uint64_t` | Unsigned integer |
| `float`, `double` | IEEE 754 float |
| `bool` | 1 byte (0/1) |
| `std::string` | Length + UTF-8 bytes |

### Container Types

| IDL Type | Serialized As |
|----------|---------------|
| `std::vector<T>` | Length + elements |
| `std::list<T>` | Length + elements |
| `std::map<K,V>` | Length + (key, value) pairs |
| `std::array<T,N>` | Fixed number of elements |
| `std::optional<T>` | Present flag + value |

### Custom Structs

```idl
struct person
{
    std::string name;
    int age;
    std::vector<std::string> hobbies;
};
```

**YAS Binary**: Optimized binary format with type information
**YAS JSON**: JSON object with field names

## 5. JSON Schema Generation

Canopy generates per-function JSON schema metadata for generated interfaces.
The JSON schema test target is added only when
`NLOHMANN_JSON_CONFIG_INSTALL_DIR` is defined at configure time.

### Generated Schema

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "calculator",
  "definitions": {
    "i_calculator_add_send": {
      "type": "object",
      "description": "Parameters for add from interface i_calculator",
      "properties": {
        "a": { "type": "integer" },
        "b": { "type": "integer" }
      },
      "required": ["a", "b"],
      "additionalProperties": false
    },
    "i_calculator_add_receive": {
      "type": "object",
      "description": "Result for add from interface i_calculator",
      "properties": {
        "result": { "type": "integer" },
        "return_value": { "type": "integer" }
      },
      "required": ["result", "return_value"],
      "additionalProperties": false
    }
  }
}
```

### Accessing Schemas

```cpp
// Get function info including schemas
auto functions = xxx::i_calculator::get_function_info();

for (const auto& func : functions)
{
    std::cout << "Function: " << func.name << "\n";
    std::cout << "Input schema: " << func.in_json_schema << "\n";
    std::cout << "Output schema: " << func.out_json_schema << "\n";
}
```

## 6. Custom Serializers

There is no public runtime registry such as
`rpc::register_custom_serializer(...)`.

Supported encodings are wired through `rpc::encoding`,
`rpc::serialise(...)` / `rpc::deserialise(...)`, and generated
`proxy_serialiser`, `stub_deserialiser`, `stub_serialiser`, and
`proxy_deserialiser` specializations. Adding a new encoding is a generator and
runtime integration task, not an application-level registration hook.

## 7. Performance Considerations

### Choosing the Right Format

| Scenario | Recommended Format |
|----------|-------------------|
| High-performance local | `yas_binary` |
| Network transfer | `yas_compressed_binary` |
| Debugging/inspection | `yas_json` |

### Size Comparison (Typical)

```
yas_binary:           100 bytes (baseline)
yas_compressed_binary: 60 bytes (compressed)
yas_json:            200 bytes (text)
```

### Speed Comparison (Typical)

```
yas_binary:           0.1 ms (fastest)
yas_compressed_binary: 0.5 ms (compression overhead)
yas_json:             1.0 ms (slowest)
```

## 8. Error Handling

### Serialization Errors

```cpp
auto error = rpc::deserialise(enc, data, obj);

if (!error.empty())
{
    // The returned string describes malformed data, unsupported encoding,
    // or a backend/type mismatch.
}
```

Generated proxy and stub paths convert serializer failures into RPC error codes
such as `PROXY_DESERIALISATION_ERROR()`, `STUB_DESERIALISATION_ERROR()`, or
`INCOMPATIBLE_SERIALISATION()` at the call boundary.

## 9. Best Practices

1. **Use binary formats** for production
2. **Use JSON for debugging** when inspecting traffic
3. **Enable compression** for large payloads over network
4. **Test all formats** during development
5. **Document format requirements** for API consumers

## 10. Next Steps

- [Protocol Buffers](protocol-buffers.md) - Cross-language serialization
- [Error Handling](../06-error-handling.md) - Error code reference
- [API Reference](../09-api-reference.md) - Complete API
