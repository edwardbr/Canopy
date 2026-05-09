# Service Object Singleton Proposal

Status: parked proposal.

This note records a proposed Canopy service-object feature. It is not the
current SGX io_uring implementation plan. The immediate SGX plan is to pass an
explicit per-enclave host capability object through the normal RPC connection
path.

## Motivation

Each Canopy service owns a zone and assigns object ids for objects in that zone.
Normal generated object ids start above zero. That leaves object id `0`
available as a reserved service-owned object.

The proposed feature is to let a service expose a singleton control object at
`(zone, object_id = 0)`. This object would provide service-level interfaces
without giving callers control over its lifetime. It is intended for control
plane and discovery-like interfaces, not for ordinary application object
ownership.

## Remote Object State

The proposal needs explicit names for the three meaningful remote-object states:

```text
!remote_object.is_set()
  null remote object

remote_object.is_set() && remote_object.get_object_id() == 0
  service object singleton

remote_object.is_set() && remote_object.get_object_id() != 0
  normal service-owned object
```

The current address representation already has an unset/default
`remote_object()` state. The proposal should not repurpose
`address_type::local == 0` as a null address type. Instead, higher-level code
should stop treating object id `0` itself as null.

Useful API names would be:

```cpp
bool remote_object::is_null() const noexcept;
bool remote_object::is_service_object() const noexcept;
bool remote_object::is_normal_object() const noexcept;
bool remote_object::is_callable() const noexcept;

rpc::remote_object rpc::remote_object::null();
rpc::expected<rpc::remote_object, std::string>
zone::with_service_object() const;
```

`is_callable()` would mean the descriptor is set and may be dispatched. Both the
service object and normal objects are callable. A null remote object is not.

## Lifetime Semantics

The service object is owned by the service. Remote callers must not be able to
hold shared ownership of it.

Proposed rules:

- object id `0` may receive optimistic `add_ref`
- object id `0` rejects shared `add_ref`
- shared `add_ref` to object id `0` should be treated as
  `FRAUDULANT_REQUEST`
- if no service object exists, calls and optimistic `add_ref` to object id `0`
  return `OBJECT_GONE`
- optimistic references to the service object keep only the route/channel state
  needed to notify the holder when the object is gone
- optimistic references never keep the service alive

On graceful service shutdown, the service should mark object id `0` gone and
send `object_released` to zones holding optimistic references. `transport_down`
is reserved for failure cases.

## Stub And Service Ownership

The current `object_stub` owns a strong `std::shared_ptr<service>`, while
`rpc::base` stores a weak stub handle so objects can discover their service
through their stub. A service-owned singleton stub would therefore create a
cycle if implemented by simply storing a normal `object_stub` strongly inside
`service`.

One possible implementation is:

- keep using `rpc::base` for service-object implementations
- add an `object_stub` policy for service-owned stubs
- service-owned stubs hold only a weak service reference
- service-owned stubs reject shared `add_ref`
- `service::get_object(object{0})` returns the service object stub when present
- service emptiness checks ignore the service-owned singleton stub itself
- service shutdown resets the service-owned `rpc::shared_ptr` while transports
  are still usable

If this becomes too awkward, introduce a separate `service_base` for service
objects that does not depend on an object-stub handle. That should be treated as
a larger design change.

## Service Object Dependencies

A service object should not normally hold `rpc::shared_ptr`s to remote objects.
It may do so if the developer deliberately provides a shutdown path that
releases them before service teardown.

Retaining remote shared ownership from a service object can keep service proxies
alive and prevent graceful shutdown. The runtime should surface this through
the existing live-proxy and leak diagnostics rather than hide it. Developers
who use this pattern are accepting explicit shutdown ordering requirements.

## Header Placement

The generic service-object handle interface should move out of
`rpc/internal/service.h`. The proposed public/internal header name is:

```text
rpc/include/rpc/handle.h
```

The existing `rpc::i_noop` can then move or be replaced by a generic
handle interface type from that header.

## Current SGX io_uring Direction

This proposal is parked for the SGX io_uring work.

For SGX io_uring, the preferred current design is to pass an explicit RPC
object through the normal `connect_to_zone` input path. That object can support:

- `i_host`, for existing SGX transport tests
- `i_io_uring_control`, for waking the host-owned io_uring SQPOLL ring if needed

The `i_io_uring_control` object should be per enclave or per io_uring
context. It should capture exactly the host-side wake capability for that
enclave's ring. It should not expose a generic syscall bridge.

The enclave should request this control interface after the normal initial
`add_ref` has completed. That keeps the standard connection protocol unchanged.

`canopy_coroutine_startup_status*` should remain a narrow startup-status
mechanism. It is suitable for fixed runtime states such as worker admission,
readiness, and failure. It is not a good place to carry extensible RPC object
capabilities.

## Implementation Areas To Revisit

If this proposal is resumed, the first audit should cover code that treats
object id `0` as null. Known areas include:

- generated and manual interface binding
- `proxy_bind_out_param`
- `demarshall_interface_proxy`
- `connect_to_zone` output descriptor handling
- transport telemetry paths
- local transport reference-count assertions
- Rust and C ABI bindings, if the protocol change is made cross-language

The goal is to make null checks use `remote_object::is_null()` and object
dispatch checks use `remote_object::is_callable()`.
