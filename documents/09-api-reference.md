<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# API Reference

Scope note:

- this is a quick-reference document for the primary C++ implementation
- names, signatures, transport families, and coroutine macros here should be
  read as C++ API guidance, not as a cross-language guarantee
- use it as a navigation aid, not as a substitute for the current headers

Who this is for:

- developers who already know the main Canopy concepts
- readers who need a quick reminder of names and signatures
- not the best first document for learning the system from scratch

Quick reference for the main Canopy APIs.

## 1. Services

The main concrete service entry points are:

- `rpc::root_service` for top-level zones that allocate their own child zone IDs
- `rpc::child_service` for hierarchical child zones that obtain new zone IDs
  through their parent path

Zones in Canopy follow an IPv6-inspired addressing model:

- **routing prefix** identifies the node or routing domain
- **subnet** identifies the zone
- **local/object address** identifies the specific object within that zone

The combination of routing prefix and subnet identifies the zone. Local/object
address `0` is reserved for the zone's service object.

### `rpc::root_service` Constructors

```cpp
// Blocking mode
rpc::root_service(const char* name, rpc::zone zone_id);

// Coroutine mode
rpc::root_service(const char* name, rpc::zone zone_id,
                  std::shared_ptr<coro::scheduler> scheduler);
```

### `rpc::child_service` Constructor

```cpp
// Blocking mode
rpc::child_service(const char* name,
                   rpc::zone zone_id,
                   rpc::destination_zone parent_zone_id);

// Coroutine mode
rpc::child_service(const char* name,
                   rpc::zone zone_id,
                   rpc::destination_zone parent_zone_id,
                   std::shared_ptr<coro::scheduler> scheduler);
```

### Zone and Object Management

```cpp
CORO_TASK(rpc::new_zone_id_result) get_new_zone_id(rpc::get_new_zone_id_params params);
rpc::object generate_new_object_id();
rpc::object get_object_id(const rpc::casting_interface* ptr);
```

### Zone Connection

```cpp
template<class InInterface, class OutInterface>
CORO_TASK(rpc::service_connect_result<OutInterface>)
connect_to_zone(
    const char* name,
    std::shared_ptr<rpc::transport> transport,
    rpc::shared_ptr<InInterface> input_interface);
```

Use `connect_to_zone(...)` when this zone is initiating a connection to another
zone, including peer connections and parent-to-child connection setup.

### Remote Zone Attachment

```cpp
template<class ParentInterface, class ChildInterface>
CORO_TASK(rpc::remote_object_result) attach_remote_zone(
    const char* name,
    std::shared_ptr<rpc::transport> transport,
    rpc::connection_settings input_descr,
    std::function<CORO_TASK(rpc::service_connect_result<ChildInterface>)(
        rpc::shared_ptr<ParentInterface>,
        std::shared_ptr<rpc::service>)> fn);
```

Use `attach_remote_zone(...)` when this zone is accepting an incoming request
from another zone and attaching that remote zone to this one in a peer-style
fashion.

### Hierarchical Child-Zone Creation

```cpp
template<class ParentInterface, class ChildInterface>
static CORO_TASK(rpc::remote_object_result) rpc::child_service::create_child_zone(
    const char* name,
    std::shared_ptr<rpc::transport> parent_transport,
    rpc::connection_settings input_descr,
    std::function<CORO_TASK(rpc::service_connect_result<ChildInterface>)(
        rpc::shared_ptr<ParentInterface>,
        std::shared_ptr<rpc::child_service>)> fn
#ifdef CANOPY_BUILD_COROUTINE
    , std::shared_ptr<coro::scheduler> scheduler
#endif
);
```

Use `create_child_zone(...)` for hierarchical child zones attaching to their
parent.

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
callable_accessor<T> get_callable() const;  // Pins local / captures remote
callable_accessor<T> operator->() const;    // Same as get_callable()
T* raw_get() const;                         // Raw dispatch pointer (may be null)
explicit operator bool() const;
```

`callable_accessor` is a lightweight object that always provides a valid dispatch
pointer.  For local objects it pins the target via `weak_ptr::lock()` so the
object cannot be destroyed while the accessor lives.  For remote objects it
copies the `interface_proxy*` which is kept alive by the optimistic count.

`callable_accessor::operator bool()` returns `true` only when the underlying
object is actually alive (not just when a proxy is present).  Use it to
distinguish local-alive from local-gone.

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
// zone_address is a versioned blob whose routing-prefix, subnet, and object-id
// layout is described by the capability header in interfaces/rpc/rpc_types.idl

struct zone_address
{
    // Accessors
    uint64_t get_routing_prefix() const;
    void set_routing_prefix(uint64_t val);
    uint64_t get_subnet() const;        // Returns the subnet value as a uint64_t
    void set_subnet(uint64_t val);
    uint64_t get_object_id() const;     // Returns the local/object value as a uint64_t, 0 for the service object
    void set_object_id(uint64_t val);

    // Comparison
    bool same_zone(const zone_address& other) const;  // Compare zone portion only
    bool is_set() const;

    // Conversion
    std::string to_string() const;  // CIDR-like notation

    // Blob conversion helpers preserve the versioned capability layout from
    // rpc_types.idl
};

struct zone { zone_address addr; };  // routing_prefix + subnet, object_id=0
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
uint64_t obj_id = addr.get_object_id();  // local/object address

// Zone type conversions
destination_zone zone::as_destination() const;
caller_zone zone::as_caller() const;
requesting_zone zone::as_requesting_zone() const;
zone remote_object::as_zone() const;  // Strip local/object address
remote_object destination_zone::with_object(object obj) const;  // Add local/object address
```

### Compatibility Note

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
std::shared_ptr<rpc::telemetry::i_telemetry_service> rpc::telemetry::get_telemetry_service();
```

### Service Creation

```cpp
rpc::telemetry::create_console_telemetry_service(service, name, test, dir);
rpc::telemetry::create_sequence_diagram_telemetry_service(service, name, test, dir);
rpc::telemetry::create_animation_telemetry_service(service, name, test, dir);
rpc::telemetry::create_multiplexing_telemetry_service(service, std::move(children));
```
