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

### Built-in Transport Families

```cpp
// In-process parent/child zones
rpc::local

// In-process DLL-backed child zones
rpc::dynamic_library         // blocking builds
rpc::libcoro_dynamic_library // coroutine builds
rpc::sgx                     // a signed dynamic library 

// Child-process transport and DLL-backed child-process composition
rpc::ipc_transport           // coroutine builds
rpc::libcoro_spsc_dynamic_dll

// Peer and stream-based transports
rpc::stream_transport
```

For the DLL and IPC variants, see `documents/transports/dynamic_library.md`.

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
// Request-response (object_id in remote_object)
CORO_TASK(int) send(
    uint64_t protocol_version,
    rpc::encoding enc,
    uint64_t transaction_id,
    rpc::caller_zone caller_zone_id,
    rpc::remote_object remote_object_id,  // includes zone and object_id
    rpc::interface_ordinal interface_id,
    rpc::method method_id,
    const rpc::span& data,
    rpc::span& response);

// Fire-and-forget
CORO_TASK(void) post(
    uint64_t protocol_version,
    rpc::encoding enc,
    uint64_t transaction_id,
    rpc::caller_zone caller_zone_id,
    rpc::remote_object remote_object_id,  // includes zone and object_id
    rpc::interface_ordinal interface_id,
    rpc::method method_id,
    const rpc::span& data);

// Interface query
CORO_TASK(int) try_cast(
    uint64_t transaction_id,
    rpc::caller_zone caller_zone_id,
    rpc::remote_object remote_object_id,  // includes zone and object_id
    rpc::interface_ordinal interface_id,
    void** object);
```

### Reference Counting

```cpp
// Add reference (object_id in remote_object)
CORO_TASK(int) add_ref(
    uint64_t protocol_version,
    rpc::remote_object remote_object_id,  // includes zone and object_id
    rpc::caller_zone caller_zone_id,
    rpc::requesting_zone requesting_zone_id,
    rpc::add_ref_options options);

// Release reference
CORO_TASK(int) release(
    uint64_t protocol_version,
    rpc::remote_object remote_object_id,  // includes zone and object_id
    rpc::caller_zone caller_zone_id,
    rpc::release_options options);
```

### Lifecycle

```cpp
// Object released (object_id in remote_object)
CORO_TASK(void) object_released(
    uint64_t protocol_version,
    rpc::remote_object remote_object_id,  // includes zone and object_id
    rpc::caller_zone caller_zone_id);

// Transport disconnected (zone-only, no object_id)
CORO_TASK(void) transport_down(
    uint64_t protocol_version,
    rpc::destination_zone destination_zone_id,  // zone-only
    rpc::caller_zone caller_zone_id);
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
// 128-bit address = 64-bit routing_prefix + 32-bit subnet_id + 32-bit object_id
// Packed representation (uint128_t / std::array<uint8_t, 16>) is CMake-configurable

struct zone_address
{
    // Accessors
    uint64_t get_routing_prefix() const;
    void set_routing_prefix(uint64_t val);
    uint64_t get_subnet() const;        // Returns subnet_id (32-bit value as uint64_t)
    void set_subnet(uint64_t val);
    uint64_t get_object_id() const;     // Returns object_id (32-bit value as uint64_t)
    void set_object_id(uint64_t val);

    // Comparison
    bool same_zone(const zone_address& other) const;  // Compare zone portion only
    bool is_set() const;

    // Conversion
    std::string to_string() const;  // CIDR-like notation

    // Packed 128-bit conversion (CMake-configurable)
    // to_packed() / from_packed() for wire serialization
};

struct zone { zone_address addr; };
struct remote_object { zone_address addr; };  // Includes object_id for i_marshaller methods
struct destination_zone { zone_address addr; };  // Zone-only (object_id=0)
struct caller_zone { zone_address addr; };
struct requesting_zone { zone_address addr; };
struct object { uint64_t id; };
struct interface_ordinal { uint64_t id; };
struct method { uint64_t id; };
```

### Conversions

```cpp
// Get the full zone_address from any zone type
const zone_address& addr = remote_object.get_address();

// Access components
uint64_t prefix = addr.get_routing_prefix();
uint64_t subnet = addr.get_subnet();
uint64_t obj_id = addr.get_object_id();

// Zone type conversions
destination_zone zone::as_destination() const;
caller_zone zone::as_caller() const;
requesting_zone zone::as_requesting_zone() const;
zone remote_object::as_zone() const;  // Strip object_id
remote_object destination_zone::with_object(object obj) const;  // Add object_id
```

### Legacy Compatibility

```cpp
// Constructor from uint64_t (sets subnet_id, routing_prefix=0, object_id=0)
rpc::zone zone{42};  // Still works for local-only mode
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
