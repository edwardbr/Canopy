<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Error Handling

Canopy provides a comprehensive error handling system with 23 distinct error codes covering memory, transport, serialization, and lifecycle errors.

For authoritative error code definitions, see `rpc/include/rpc/internal/error_codes.h`.

## Error Code Offsets

Error codes can be customized using offset functions to avoid conflicts with application-specific error codes:

```cpp
// Customize error code base values
rpc::error::set_OK_val(0);              // Set OK value (default 0)
rpc::error::set_offset_val(100);        // Set offset magnitude
rpc::error::set_offset_val_is_negative(false);  // Offset direction
```

The **±** notation in tables below indicates the error value is `offset ± ordinal`, where the offset can be configured as positive or negative.

## 1. Error Code Reference

### Success

```cpp
rpc::error::OK()  // Configured via set_OK_val(), default = 0
```

### Memory Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `OUT_OF_MEMORY` | ±1 | Service has no more memory |
| `NEED_MORE_MEMORY` | ±2 | Call needs more memory for out parameters |

### Data Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `INVALID_DATA` | ±4 | Invalid data received |
| `INVALID_METHOD_ID` | ±6 | Wrong method ordinal |
| `INVALID_INTERFACE_ID` | ±7 | Interface not implemented |
| `INVALID_CAST` | ±8 | Unable to cast interface |

### Transport Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `TRANSPORT_ERROR` | ±5 | Custom transport error |
| `SERVICE_PROXY_LOST_CONNECTION` | ±21 | Channel unavailable |

### Zone Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `ZONE_NOT_SUPPORTED` | ±9 | Zone inconsistent with proxy |
| `ZONE_NOT_INITIALISED` | ±10 | Zone not ready |
| `ZONE_NOT_FOUND` | ±11 | Zone not found |

### Object Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `OBJECT_NOT_FOUND` | ±12 | Invalid object ID |
| `OBJECT_GONE` | ±23 | Object no longer exists |

### Version/Compatibility Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `INVALID_VERSION` | ±13 | Unsupported RPC version |
| `INCOMPATIBLE_SERVICE` | ±17 | Service incompatibility |
| `INCOMPATIBLE_SERIALISATION` | ±18 | Unsupported encoding format |

### Serialization Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `PROXY_DESERIALISATION_ERROR` | ±15 | Proxy deserialization failed |
| `STUB_DESERIALISATION_ERROR` | ±16 | Stub deserialization failed |

### Other Errors

| Error Code | Value | Description |
|------------|-------|-------------|
| `SECURITY_ERROR` | ±3 | Security-specific issue |
| `EXCEPTION` | ±14 | Uncaught exception |
| `REFERENCE_COUNT_ERROR` | ±19 | Ref count issue |
| `UNABLE_TO_CREATE_SERVICE_PROXY` | ±20 | Proxy creation failed |
| `CALL_CANCELLED` | ±22 | Remote call cancelled |

## 2. Error Checking Patterns

### Basic Error Check

```cpp
auto error = CO_AWAIT calculator_->add(10, 20, result);

if (error == rpc::error::OK())
{
    std::cout << "Result: " << result << "\n";
}
else
{
    std::cerr << "Error: " << static_cast<int>(error) << "\n";
}
```

### Switch on Error

```cpp
auto error = CO_AWAIT calculator_->divide(a, b, result);

switch (error)
{
    case rpc::error::OK():
        std::cout << "Result: " << result << "\n";
        break;
    case rpc::error::INVALID_DATA():
        std::cerr << "Invalid input\n";
        break;
    case rpc::error::OBJECT_GONE():
        std::cerr << "Calculator was destroyed\n";
        break;
    default:
        std::cerr << "Unknown error: " << static_cast<int>(error) << "\n";
        break;
}
```

### Error Helper Functions

Canopy provides a built-in error-to-string converter:

```cpp
const char* rpc::error::to_string(int err);
```

**Example usage**:

```cpp
auto error = CO_AWAIT calculator_->add(10, 20, result);
if (error != rpc::error::OK())
{
    std::cerr << "Error: " << rpc::error::to_string(error) << "\n";
}
```

## 3. Propagating Errors

### Simple Propagation

```cpp
CORO_TASK(error_code) wrapper_operation(int input, [out] int& output)
{
    auto error = CO_AWAIT calculator_->process(input, output);
    CO_RETURN error;  // Propagate up
}
```

### Error Transformation

```cpp
CORO_TASK(error_code) safe_operation(int input, [out] int& output)
{
    auto error = CO_AWAIT calculator_->risky_operation(input, output);

    if (error == rpc::error::INVALID_DATA())
    {
        // Transform to a more specific error
        CO_RETURN rpc::error::INVALID_DATA();  // Or a custom error
    }

    CO_RETURN error;
}
```

### Error Accumulation

```cpp
CORO_TASK(error_code) multi_step_operation()
{
    int temp1, temp2, temp3;
    auto error;

    error = CO_AWAIT step1(temp1);
    if (error != rpc::error::OK())
        CO_RETURN error;

    error = CO_AWAIT step2(temp1, temp2);
    if (error != rpc::error::OK())
        CO_RETURN error;

    error = CO_AWAIT step3(temp2, temp3);
    if (error != rpc::error::OK())
        CO_RETURN error;

    CO_RETURN rpc::error::OK();
}
```

## 4. Logging Errors

### Using Logging Macros

```cpp
// Debug level
RPC_DEBUG("Operation completed with result {}", result);

// Info level
RPC_INFO("Calculator operation add({}, {}) = {}", a, b, result);

// Warning level
RPC_WARNING("Invalid data received: {}", data);

// Error level
RPC_ERROR("Transport error: {}", static_cast<int>(error));

// Critical level
RPC_CRITICAL("Fatal error in service {}", service_name);
```

### Conditional Logging

```cpp
if (error != rpc::error::OK())
{
    RPC_ERROR("Operation failed: {} (code={})",
              error_to_string(static_cast<int>(error)),
              static_cast<int>(error));
}
```

## 5. Assertions

### Runtime Assertions

```cpp
RPC_ASSERT(ptr != nullptr);
RPC_ASSERT(count > 0);
RPC_ASSERT(error == rpc::error::OK());
```

### Assertion Modes

**Debug build** (aborts with assert message):
```cpp
#define RPC_ASSERT(x) \
    if (!(x))         \
        assert(!"error failed " #x);
```

**Release build** (aborts immediately):
```cpp
#define RPC_ASSERT(x) \
    if (!(x))         \
        std::abort();
```

**With thread-local logging** (dumps logs before abort):
```cpp
#define RPC_ASSERT(x) \
    if (!(x))         \
        rpc::thread_local_dump_on_assert("RPC_ASSERT failed: " #x, __FILE__, __LINE__); \
        std::abort();
```

## 6. Transport Status Handling

```cpp
auto status = transport_->get_status();

switch (status)
{
    case transport_status::CONNECTING:
        // Wait for connection
        break;
    case transport_status::CONNECTED:
        // Ready for operations
        break;
    case transport_status::DISCONNECTING:
        // Beginning to shut down a close signal is being sent or recieved
        break;
    case transport_status::DISCONNECTED:
        // Terminal state close signal has been acknowleged, or there is a terminal failure, no further traffic allowed
        CO_RETURN rpc::error::TRANSPORT_ERROR();
}
```

## 7. Object Lifecycle Errors

### Object Gone

```cpp
auto error = CO_AWAIT calculator_->add(10, 20, result);

if (error == rpc::error::OBJECT_GONE())
{
    // Object was destroyed while call was in flight
    // Create new calculator or reconnect
    calculator_ = create_new_calculator();
}
```

### Object Not Found

```cpp
auto error = CO_AWAIT proxy_->get_object(object_id, result);

if (error == rpc::error::OBJECT_NOT_FOUND())
{
    // Invalid object ID or wrong zone
    // Verify object_id and zone configuration
}
```

## 8. Version Mismatch

```cpp
auto error = CO_AWAIT proxy_->call(method_id, input, output);

if (error == rpc::error::INVALID_VERSION())
{
    // Protocol version mismatch
    // Negotiate version or upgrade client/server
    RPC_ERROR("Version mismatch - client={}, server={}",
              client_version, server_version);
}
```

## 9. Best Practices

1. **Always check return values** from RPC calls
2. **Handle OBJECT_GONE** - object may be destroyed during call
3. **Use logging** for error context
4. **Use assertions** for programmer errors
5. **Transform errors** at appropriate layers
6. **Don't swallow errors** - propagate or handle explicitly

## 10. Next Steps

- [Telemetry](07-telemetry.md) - Debug with comprehensive logging
- [Memory Management](architecture/04-memory-management.md) - Understanding lifecycle
- [API Reference](09-api-reference.md) - Complete error code list
