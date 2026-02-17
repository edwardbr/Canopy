<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Migration Guide

This guide helps you migrate from older Canopy versions or from other RPC frameworks.

## 1. Upgrading Canopy Versions

### From v1.x to v2.x

#### Breaking Changes

1. **Smart Pointer Renames**
   ```cpp
   // v1.x
   rpc::proxy_ptr<T>
   rpc::stub_ptr<T>

   // v2.x
   rpc::shared_ptr<T>       // for proxies
   rpc::weak_ptr<T>
   rpc::optimistic_ptr<T>
   ```

2. **Error Code API**
   ```cpp
   // v1.x
   return RPC_OK;
   return RPC_ERROR;

   // v2.x
   CO_RETURN rpc::error::OK();
   CO_RETURN rpc::error::INVALID_DATA();
   ```

3. **Service Creation**
   ```cpp
   // v1.x
   auto service = new rpc::service("name", zone_id);

   // v2.x
   auto service = std::make_shared<rpc::service>("name", zone_id);
   ```

4. **Transport Connection**
   ```cpp
   // v1.x
   transport->connect_sync();
   transport->call_sync();

   // v2.x
   auto error = CO_AWAIT transport->connect();
   auto error = CO_AWAIT transport->send(...);
   ```

#### Migration Steps

1. Update smart pointer usage throughout codebase
2. Replace error code constants with new enum values
3. Update service creation to use make_shared
4. Convert synchronous calls to coroutine pattern
5. Update CMake dependencies

### From Blocking to Coroutine Mode

#### Code Changes

```cpp
// Blocking code
error_code my_method(int input, int& output)
{
    return worker_->process(input, output);
}

// Coroutine code
CORO_TASK(error_code) my_method(int input, int& output)
{
    return CO_AWAIT worker_->process(input, output);
}
```

#### Build Changes

```bash
# From
cmake --preset Debug

# To
cmake --preset Coroutine_Debug
```

#### Scheduler Integration

```cpp
#ifdef CANOPY_BUILD_COROUTINE
auto scheduler = coro::io_scheduler::make_shared(...);
auto service = std::make_shared<rpc::service>("name", zone_id, scheduler);
#else
auto service = std::make_shared<rpc::service>("name", zone_id);
#endif
```

## 2. Migration from Other RPC Frameworks

### From gRPC

#### Conceptual Differences

| Aspect | gRPC | Canopy |
|--------|------|--------|
| Interface Definition | .proto | .idl |
| Code Generation | protoc | rpc_generator |
| Transport | HTTP/2 | Pluggable |
| Serialization | Protobuf | Multiple (YAS, JSON, Protobuf) |
| Threading | Callbacks | Coroutines/Blocking |

#### IDL to Proto Conversion

```idl
// Canopy IDL
namespace math
{
    interface i_calculator
    {
        error_code add(int a, int b, [out] int& result);
    };
}
```

```protobuf
// gRPC proto
syntax = "proto3";

package math;

service Calculator {
    rpc Add(AddRequest) returns (AddResponse);
}

message AddRequest {
    int32 a = 1;
    int32 b = 2;
}

message AddResponse {
    int32 result = 1;
}
```

#### Implementation Pattern

```cpp
// Canopy pattern
class calculator_impl : public i_calculator
{
    CORO_TASK(error_code) add(int a, int b, [out] int& result) override
    {
        result = a + b;
        CO_RETURN error::OK();
    }
};
```

#### Key Differences to Handle

1. **No stub/proxy separation** in user code (handled by generator)
2. **Error handling via return values** not exceptions
3. **Zone concept** for distributed execution contexts
4. **dont cast std::shared_ptr with rpc::shared_ptr** - don't mix pointer types

### From Thrift

#### Conceptual Differences

| Aspect | Thrift | Canopy |
|--------|--------|--------|
| Interface Definition | .thrift | .idl |
| Code Generation | thrift compiler | rpc_generator |
| Transport | Multiple (socket, HTTP, etc.) | Pluggable |
| Serialization | Binary, JSON, compact | YAS, JSON, protobuf |
| Threading | Worker threads | Coroutines/Blocking |

#### Migration Steps

1. Convert `.thrift` files to `.idl` format
2. Update generated code integration
3. Replace Thrift client/server with Canopy equivalents
4. Update error handling (exceptions → error codes)

#### Example Conversion

```thrift
// Thrift
service Calculator {
    i32 add(1:i32 a, 2:i32 b)
}
```

```idl
// Canopy
interface i_calculator
{
    error_code add(int a, int b, [out] int& result);
}
```

### From COM/DCOM

#### Conceptual Differences

| Aspect | COM | Canopy |
|--------|-----|--------|
| Interface Definition | IDL (similar) | IDL |
| Reference Counting | IUnknown | rpc::shared_ptr |
| Marshalling | Proxy/stub DLLs | Generated code |
| Registration | Registry | No registry |
| Apartments | N/A | Zones |

#### Migration Steps

1. Convert COM interfaces to Canopy IDL
2. Replace IUnknown methods with casting_interface
3. Replace AddRef/Release with shared_ptr
4. Remove registry registration
5. Update apartment model to zone model

#### Example Conversion

```cpp
// COM
class Calculator : public ICalculator
{
    STDMETHODIMP Add(int a, int b, int* result) override
    {
        *result = a + b;
        return S_OK;
    }
};
```

```cpp
// Canopy
class calculator : public rpc::base<calculator, i_calculator>
{
    CORO_TASK(error_code) add(int a, int b, [out] int& result) override
    {
        result = a + b;
        CO_RETURN error::OK();
    }
};
```

## 3. CMake Migration

### Old Style

```cmake
# Pre-CMakePresets
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_COROUTINE=OFF ..
make -j4
```

### New Style

```bash
# With CMakePresets
cmake --preset Debug
cmake --build build --parallel 4
```

### Build Target Changes

```cmake
# Old
add_executable(my_app main.cpp)
target_link_libraries(my_app rpc)

# New
CanopyGenerate(my_interface ...)  # Generate code first
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE rpc_host my_interface_idl)
```

## 4. Common Migration Issues

### Issue: Object Not Found After Migration

**Cause**: Reference counting behavior difference

**Solution**: Ensure proper shared_ptr lifecycle

### Issue: Compilation Errors in Generated Code

**Cause**: Missing includes or build order

**Solution**: Ensure generated code is built before usage

### Issue: Transport Connection Failures

**Cause**: Zone ID mismatch or transport setup change

**Solution**: Verify zone IDs match and transport is properly configured

### Issue: Error Codes Not Matching

**Cause**: Different error code values

**Solution**: Use rpc::error:: enum values

## 5. Recommended Migration Order

1. **Week 1**: Setup new Canopy build system alongside old
2. **Week 2**: Convert one interface to IDL and generate code
3. **Week 3**: Migrate one client or service to use Canopy
4. **Week 4**: Test and iterate
5. **Week 5+**: Continue migrating remaining interfaces

## 6. Testing Strategy

### Parallel Testing

```bash
# Run old and new implementations
./old_implementation --test
./new_implementation --test

# Compare results
diff <(./old_implementation --test) <(./new_implementation --test)
```

### Compatibility Testing

```cpp
// Test old wire format compatibility
auto serialized = old_serialize(data, old_format);
auto result = rpc::deserialise(rpc::encoding::yas_json, serialized, data);
```

## 7. Rollback Plan

1. Keep old build system available
2. Use feature flags to switch between implementations
3. Test thoroughly before full migration
4. Have rollback scripts ready

## 8. Getting Help

- Review [Examples](10-examples.md) for patterns
- Check [Best Practices](11-best-practices.md) for guidance
- Use [Telemetry](07-telemetry.md) for debugging
- Enable logging to trace issues

## 9. Version Compatibility

| From Version | To Version | Notes |
|--------------|------------|-------|
| v1.0 | v2.0 | Breaking changes |
| v1.1 | v2.0 | Breaking changes |
| v2.0 | v2.1 | Minor changes |
| v2.1 | v2.2 | Compatible |
