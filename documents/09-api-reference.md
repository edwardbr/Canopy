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
struct rpc::service_config
{
    rpc::zone initial_zone{rpc::DEFAULT_PREFIX};
};

// Blocking mode
static std::shared_ptr<rpc::root_service> rpc::root_service::create(
    const char* name,
    rpc::zone zone_id);

static std::shared_ptr<rpc::root_service> rpc::root_service::create(
    const char* name,
    const rpc::service_config& config);

// Coroutine mode
static std::shared_ptr<rpc::root_service> rpc::root_service::create(
    const char* name,
    rpc::zone zone_id,
    const std::shared_ptr<coro::scheduler>& scheduler);

static std::shared_ptr<rpc::root_service> rpc::root_service::create(
    const char* name,
    const rpc::service_config& config,
    const std::shared_ptr<coro::scheduler>& scheduler);
```

### `rpc::child_service` Constructor

User code normally creates child zones through
`rpc::child_service::create_child_zone(...)` or a hierarchical transport helper.
The concrete constructor exists for transport implementations.

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
rpc::object get_object_id(const rpc::shared_ptr<rpc::casting_interface>& ptr) const;
rpc::encoding get_default_encoding() const;
void set_default_encoding(rpc::encoding enc);
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
T& operator*() const;     // Dereference
T* operator->() const;    // Arrow operator
explicit operator bool() const;  // Check if valid
```

### Reference Counting

```cpp
long use_count() const;     // Current shared reference count
bool unique() const;        // True if only reference
void reset();               // Release reference
```

### Conversion

```cpp
auto derived = CO_AWAIT rpc::dynamic_pointer_cast<Derived>(base);
auto base = rpc::static_pointer_cast<Base>(derived);
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
auto [error, opt] = CO_AWAIT rpc::make_optimistic(shared);
```

### Access

```cpp
callable_accessor<T> get_callable() const;  // Pins local / captures remote
callable_accessor<T> operator->() const;    // Same as get_callable()
T* raw_get() const;                         // Direct pointer, may be null
T* get_unsafe_only_for_testing() const;     // Unsafe direct pointer for tests/comparison
bool expired() const;
bool is_null() const;
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
rpc::dynamic_library                         // blocking builds
rpc::libcoro_host_scheduled_dynamic_library  // coroutine builds, host scheduler
rpc::libcoro_dll_scheduled_dynamic_library   // coroutine builds, DLL scheduler
rpc::sgx                                     // SGX-style hierarchical enclave transport

// Child-process transport and DLL-backed child-process composition
rpc::ipc_transport                   // coroutine builds, process host helper
rpc::libcoro_spsc_dynamic_dll        // loaded DLL runtime over SPSC queues

// Peer and stream-based transports
rpc::stream_transport
rpc::tcp           // TCP stream/RPC factory helpers
rpc::spsc_queue    // SPSC queue-pair stream/RPC factory helpers, coroutine only
rpc::io_uring      // Linux loopback io_uring stream/RPC factory helpers, coroutine only
```

For DLL and IPC details, see `documents/transports/dynamic_library.md`. The
direct `canopy_ipc_child_process` executable is currently disabled; the current
process-hosted DLL path uses `canopy_ipc_child_host_process`.

The high-level stream factories prefer typed configuration objects generated
from `connection_factory_config.idl`, primarily
`rpc::connection_factory_config::stream_factory_options`. JSON overloads are still
available for config files and command-line overlays; they validate exact nested
option names against the generated schema before converting to typed options or
transport-specific settings.

### Status

```cpp
transport_status get_status() const;
void set_status(rpc::transport_status new_status);
rpc::zone get_zone_id() const;
rpc::zone get_adjacent_zone_id() const;
```

### Connection

```cpp
CORO_TASK(rpc::connect_result) connect(
    std::shared_ptr<rpc::object_stub> stub,
    rpc::connection_settings input_descr);

CORO_TASK(int) accept();
```

### Messaging

```cpp
CORO_TASK(rpc::send_result) send(rpc::send_params params);
CORO_TASK(void) post(rpc::post_params params);
CORO_TASK(rpc::standard_result) try_cast(rpc::try_cast_params params);
CORO_TASK(rpc::handshake_result) handshake(rpc::handshake_params params);
```

### Reference Counting

```cpp
CORO_TASK(rpc::standard_result) add_ref(rpc::add_ref_params params);
CORO_TASK(rpc::standard_result) release(rpc::release_params params);
```

### Lifecycle

```cpp
CORO_TASK(void) object_released(rpc::object_released_params params);
CORO_TASK(void) transport_down(rpc::transport_down_params params);
CORO_TASK(rpc::new_zone_id_result) get_new_zone_id(rpc::get_new_zone_id_params params);
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

`INVALID_VERSION` covers unsupported RPC versions, generated IDL fingerprints,
and message schemas. `FRAUDULANT_REQUEST` is reserved for security/protocol
violations such as authenticated tamper, replay, downgrade attempts, impossible
sequencing, or invalid request-scoped capability handoff; do not use it for an
unknown fingerprint by itself.

### Error Helper

```cpp
const char* rpc::error::to_string(int error_code);
```

## 7. Zone Types

```cpp
// zone_address is a versioned blob whose routing-prefix, subnet, and object-id
// layout is described by the capability header in interfaces/rpc/rpc_types.idl

struct zone_address
{
    static rpc::expected<zone_address, std::string> create(const zone_address_args& args);
    static rpc::expected<zone_address, std::string> from_blob(std::vector<uint8_t> raw_blob);

    const std::vector<uint8_t>& get_blob() const;
    uint8_t get_version() const;
    rpc::address_type get_address_type() const;
    uint16_t get_port() const;
    std::vector<uint8_t> get_routing_prefix() const;
    uint64_t get_subnet() const;
    uint64_t get_object_id() const;  // 0 for the service object

    rpc::expected<void, std::string> set_subnet(uint64_t val);
    rpc::expected<void, std::string> set_object_id(uint64_t val);

    // Comparison
    bool same_zone(const zone_address& other) const;  // Compare zone portion only
    bool is_set() const;

    // Conversion
    zone_address zone_only() const;
    rpc::expected<zone_address, std::string> with_object(uint64_t obj) const;
};

struct zone;           // zone-only wrapper, object_id=0
using destination_zone = zone;
using caller_zone = zone;
using requesting_zone = zone;

struct remote_object;  // zone + object identity for marshaller methods
struct object { uint64_t id; };
struct interface_ordinal { uint64_t id; };
struct method { uint64_t id; };
```

### Conversions

```cpp
const rpc::zone_address& addr = remote_object.get_address();

// Access components
auto prefix = addr.get_routing_prefix();
uint64_t subnet = addr.get_subnet();
uint64_t obj_id = addr.get_object_id();  // local/object address

// Zone type conversions
rpc::zone zone_only = remote_object.as_zone();
auto object_result = zone_only.with_object(rpc::object{7});
if (!object_result)
{
    return rpc::error::INVALID_DATA();
}
rpc::remote_object object_ref = *object_result;
```

### Local Default Prefix

Use `rpc::DEFAULT_PREFIX` as the local default zone prefix, then let a root
service or allocator assign concrete child subnets.

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
template<class OutputBlob = std::vector<std::uint8_t>, typename T>
OutputBlob serialise(const T& obj, rpc::encoding enc);

template<typename T>
std::string deserialise(rpc::encoding enc, const rpc::byte_span& data, T& obj);

template<typename T>
uint64_t get_saved_size(const T& obj, rpc::encoding enc);
```

### Encodings

```cpp
rpc::encoding::not_set;
rpc::encoding::yas_binary;
rpc::encoding::yas_compressed_binary;
rpc::encoding::yas_json;
rpc::encoding::protocol_buffers;
rpc::encoding::nanopb;
rpc::encoding::canonical_crypto;  // when CANOPY_BUILD_CANONICAL_CRYPTO=ON
```

`rpc::effective_encoding(...)` maps between `protocol_buffers` and `nanopb`
when only one protobuf-compatible backend is compiled.

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

// Get JSON schema for the generated interface or struct. Currently available
// for rpc::encoding::yas_json.
static std::string get_schema();
static std::string get_schema(rpc::encoding encoding);
static constexpr const char* get_inner_schema();

// Get interface ID for version
static rpc::interface_ordinal get_id(uint64_t rpc_version);
```

### Factory Functions

```cpp
// Create proxy from object proxy
static rpc::shared_ptr<interface_type> create(
    std::shared_ptr<rpc::object_proxy>&& object_proxy);

// Create a local optimistic-call proxy for local optimistic_ptr conversion
static std::shared_ptr<rpc::local_proxy<interface_type>> create_local_proxy(
    const rpc::weak_ptr<interface_type>& ptr);
```

## 12. Telemetry

### Getting Service

```cpp
std::shared_ptr<rpc::telemetry::i_telemetry_service> rpc::telemetry::get_telemetry_service();
```

### Service Creation

```cpp
std::shared_ptr<rpc::telemetry::i_telemetry_service> service;
bool ok = rpc::telemetry::create_console_telemetry_service(service, suite, test, dir);
ok = rpc::telemetry::create_sequence_diagram_telemetry_service(service, suite, test, dir);
ok = rpc::telemetry::create_animation_telemetry_service(service, suite, test, dir);

std::vector<std::shared_ptr<rpc::telemetry::i_telemetry_service>> children;
ok = rpc::telemetry::create_multiplexing_telemetry_service(service, std::move(children));
ok = rpc::telemetry::create_global_multiplexing_telemetry_service(std::move(children));
```
