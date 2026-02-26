<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# API Reference

Quick reference for the main Canopy APIs.

## 1. Service

### Constructors

```cpp
// Blocking mode
rpc::service(const char* name, rpc::zone zone_id);

// Coroutine mode
rpc::service(const char* name, rpc::zone zone_id,
             std::shared_ptr<coro::scheduler> scheduler);
```

### Zone and Object Management

```cpp
rpc::zone generate_new_zone_id();
rpc::object generate_new_object_id();
rpc::object get_object_id(const rpc::casting_interface* ptr);
```

### Zone Connection

```cpp
template<typename ServiceProxyType, typename... Args>
error_code connect_to_zone(
    const char* name,
    std::shared_ptr<rpc::transport> transport,
    const rpc::shared_ptr<ServiceProxyType>& service_proxy,
    Args&&... args);
```

### Remote Zone Attachment

```cpp
template<typename InterfaceType, typename... Args>
error_code attach_remote_zone(
    const char* name,
    std::shared_ptr<rpc::transport> transport,
    const rpc::interface_descriptor& input_descr,
    const rpc::interface_descriptor& output_descr,
    SetupCallback<InterfaceType, Args...> setup);
```

## 2. rpc::shared_ptr

### Creation

```cpp
// Factory function (recommended)
template<typename T, typename... Args>
rpc::shared_ptr<T> make_shared(Args&&... args);

// From existing pointer
rpc::shared_ptr<T> ptr(new T());

// From another shared_ptr
rpc::shared_ptr<T> ptr = other_ptr;
```

### Pointers and Access

```cpp
T* get() const;           // Raw pointer (may be null)
T* get_nullable() const;  // Raw pointer (never throws)
T& operator*() const;     // Dereference
T* operator->() const;    // Arrow operator
explicit operator bool() const;  // Check if valid
```

### Reference Counting

```cpp
int32_t use_count() const;  // Current reference count
bool unique() const;        // True if only reference
void reset();               // Release reference
```

### Conversion

```cpp
// Dynamic cast to derived type
template<typename U>
rpc::shared_ptr<U> dynamic_pointer_cast();

// Static cast
template<typename U>
rpc::shared_ptr<U> static_pointer_cast();
```

## 3. rpc::weak_ptr

### Creation

```cpp
rpc::shared_ptr<T> shared = rpc::make_shared<T>();
rpc::weak_ptr<T> weak(shared);
```

### Access

```cpp
rpc::shared_ptr<T> lock();  // Upgrade to shared_ptr, may return null
bool expired() const;       // Check if object is destroyed
```

## 4. rpc::optimistic_ptr

### Creation

```cpp
rpc::shared_ptr<T> shared = rpc::make_shared<T>();
rpc::optimistic_ptr<T> opt;

error_code error = rpc::make_optimistic(shared, opt);
```

### Access

```cpp
T* operator->() const;     // Arrow operator (may be null)
T* get() const;            // Raw pointer (may be null)
explicit operator bool() const;
```

## 5. Transport

### Status

```cpp
transport_status get_status() const;
bool is_connected() const;
```

### Connection

```cpp
CORO_TASK(int) connect();  // Establish connection
```

### Messaging

```cpp
// Request-response
CORO_TASK(int) send(
    uint64_t protocol_version,
    rpc::encoding enc,
    uint64_t transaction_id,
    const rpc::span& data,
    rpc::span& response);

// Fire-and-forget
CORO_TASK(int) post(
    uint64_t protocol_version,
    rpc::encoding enc,
    const rpc::span& data);

// Interface query
CORO_TASK(int) try_cast(
    uint64_t transaction_id,
    rpc::interface_ordinal interface_id,
    void** object);
```

### Reference Counting

```cpp
CORO_TASK(int) add_ref(uint64_t transaction_id,
                       rpc::add_ref_options options);
CORO_TASK(int) release(uint64_t transaction_id,
                       rpc::release_options options);
```

### Lifecycle

```cpp
void object_released(rpc::object object_id);
void transport_down();
```

## 6. Error Codes

### Success

```cpp
rpc::error::OK()  // = 0
```

### Common Errors

```cpp
rpc::error::OUT_OF_MEMORY()
rpc::error::INVALID_DATA()
rpc::error::TRANSPORT_ERROR()
rpc::error::OBJECT_NOT_FOUND()
rpc::error::OBJECT_GONE()
rpc::error::INVALID_VERSION()
rpc::error::INCOMPATIBLE_SERIALISATION()
```

### Error Helper

```cpp
const char* to_string(int error_code);
```

## 7. Zone Types

```cpp
struct zone { uint64_t id; };
struct destination_zone { uint64_t id; };
struct caller_zone { uint64_t id; };
struct requesting_zone { uint64_t id; };
struct object { uint64_t id; };
struct interface_ordinal { uint64_t id; };
struct method { uint64_t id; };
```

### Conversions

```cpp
destination_zone zone::as_destination() const;
caller_zone zone::as_caller() const;
requesting_zone zone::as_requesting_zone() const;
zone destination_zone::as_zone() const;
```

## 8. Interface Macros

### Coroutine Macros

```cpp
CORO_TASK(return_type)    // Function return type
CO_RETURN value;          // Return from coroutine
CO_AWAIT expr;            // Suspend until complete
```

### Example

```cpp
CORO_TASK(error_code) my_method(int input, [out] int& output)
{
    auto error = CO_AWAIT other_service_->process(input, output);
    CO_RETURN error;
}
```

## 9. Serialization

### Functions

```cpp
template<typename T, rpc::encoding Enc>
std::vector<uint8_t> serialise(const T& obj);

template<typename T, rpc::encoding Enc>
error_code deserialise(const rpc::span& data, T& obj);

template<typename T, rpc::encoding Enc>
size_t get_saved_size(const T& obj);
```

### Encodings

```cpp
rpc::encoding::yas_binary;
rpc::encoding::yas_compressed_binary;
rpc::encoding::yas_json;
rpc::encoding::protocol_buffers;
```

## 10. Logging

### Macros

```cpp
RPC_DEBUG(format, args...);
RPC_TRACE(format, args...);
RPC_INFO(format, args...);
RPC_WARNING(format, args...);
RPC_ERROR(format, args...);
RPC_CRITICAL(format, args...);
```

### Assertions

```cpp
RPC_ASSERT(condition);
```

## 11. IDL-Generated

### Interface Methods

```cpp
// Get function info including JSON schemas
static std::vector<rpc::function_info> get_function_info();

// Get interface ID for version
static uint64_t get_id(uint64_t rpc_version);
```

### Factory Functions

```cpp
// Create proxy from object proxy
static rpc::shared_ptr<interface_type> create(
    std::shared_ptr<rpc::object_proxy>&& object_proxy);

// Create stub from implementation
static std::shared_ptr<stub_type> create(
    const rpc::shared_ptr<interface_type>& target,
    std::weak_ptr<rpc::object_stub> target_stub);
```

## 12. Telemetry

### Getting Service

```cpp
std::shared_ptr<rpc::i_telemetry_service> get_telemetry_service();
```

### Service Creation

```cpp
rpc::console_telemetry_service::create(service, name, test, dir);
rpc::sequence_diagram_telemetry_service::create(service, name, test, dir);
rpc::animation_telemetry_service::create(service, name, test, dir);
rpc::multiplexing_telemetry_service::create(service, children);
```
