<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SGX Enclave Transport (rpc::sgx)

Scope note:

- this document describes the current C++ SGX transport/runtime integration
- the SGX transport model is a C++-specific transport family and depends on the
  SGX build/toolchain support in the current tree
- see [C++ Status](../status/cpp.md), [Rust Status](../status/rust.md), and
  [JavaScript Status](../status/javascript.md) for implementation scope

Secure communication between host application and Intel SGX enclaves.

Security note:

- the SGX host process, shared queues, ECALL arguments, OCALL results, and
  scheduling behaviour must be treated as untrusted
- start with [SGX Enclave Threat Model](../security/sgx-threat-model.md) and
  [SGX Runtime Lifecycle Security](../security/sgx-runtime-lifecycle.md) before
  changing enclave startup, worker admission, queue handling, or shutdown

## When to Use

- Secure computation with hardware guarantees
- Protecting sensitive data processing
- Confidential computing scenarios

## Requirements

- Intel SGX SDK
- `CANOPY_BUILD_ENCLAVE=ON`
- Coroutine SGX additionally requires a coroutine build and the SGX simulation or
  hardware preset that enables `c++/transports/sgx_coroutine`

## Architecture

The SGX path is a C++ hierarchical transport family. It has two important
variants:

- the blocking SGX transport, which uses SGX boundary calls directly for RPC
  traffic
- the coroutine SGX transport, which hosts a normal
  `rpc::stream_transport::transport` over queue-backed streams and uses ECALLs
  to provide enclave runtime threads

This page should be read as specialised host/enclave boundary implementation
guidance, not as a universal Canopy transport capability.

```
┌─────────────────────────────────────────┐
│                 Host                     │
│  ┌───────────────────────────────────┐  │
│  │         Host Application          │  │
│  │  ┌─────────────────────────────┐  │  │
│  │  │    host_service_proxy       │  │  │
│  │  └──────────────┬──────────────┘  │  │
│  └─────────────────┼──────────────────┘  │
│                    │ OCALL               │
│ ┌──────────────────┴──────────────────┐ │
│ │           Enclave Boundary          │ │
│ │  ┌─────────────────────────────┐    │ │
│  │  │    enclave_service_proxy   │    │ │
│  │  └──────────────┬──────────────┘    │ │
│  │                 │ ECALL              │ │
│  │  ┌─────────────────────────────┐    │ │
│  │  │      Enclave Code           │    │ │
│  │  └─────────────────────────────┘    │ │
│  └────────────────────────────────────┘ │
└─────────────────────────────────────────┘
```

## Hierarchical Transport Pattern

SGX enclave transport implements the standard hierarchical transport pattern used
by the current parent/child transport families: local, SGX, and the in-process
DLL transports.

### Key Features:
- **Circular dependency by design**: Host and enclave zones keep each other alive
- **Stack-based lifetime protection**: Prevents use-after-free during ECALL/OCALL
- **Safe disconnection protocol**: Coordinated cleanup across enclave boundary
- **Thread-safe with `stdex::member_ptr`**: Protects concurrent ECALL/OCALL

### SGX-Specific Behavior:
- `child_transport` in host zone calls into enclave via ECALL
- `parent_transport` in enclave zone calls back to host via OCALL
- Stack-based `shared_ptr` protects transport during boundary crossing
- Serialization/deserialization happens automatically for boundary crossing

**See `documents/transports/hierarchical.md` for complete architecture details and safety properties.**

## Coroutine SGX Transport

The coroutine SGX transport should behave like the coroutine SPSC dynamic DLL
transport, with SGX-specific setup and runtime entry around it.

The stable layering is:

```text
Host service
  -> rpc::sgx::coro::enclave::transport
  -> SGX ECALL runtime boundary
  -> streaming::spsc_queue::stream
  -> rpc::stream_transport::transport
  -> enclave child_service and user implementation
```

The SGX wrapper is responsible for:

- creating and destroying the enclave
- allocating and validating shared queue memory
- entering the enclave through the init ECALL and worker ECALLs
- keeping enclave runtime threads available while coroutine RPC work is pending
- exposing an SPSC stream to the normal stream transport

The generic stream transport is responsible for:

- RPC message envelopes
- add-ref and release messages
- call responses
- route and pass-through propagation
- graceful `close_connection_send` / `close_connection_ack`
- common stream cleanup

Do not reintroduce a separate SGX-specific streamed RPC protocol unless the
generic stream transport cannot express a required SGX property. The current
coroutine direction is deliberately close to
`rpc::libcoro_spsc_dynamic_dll`: the enclave runtime supplies the execution
environment, while `rpc::stream_transport::transport` supplies the RPC transport
semantics.

The queue-backed stream is still host-controlled memory. The enclave must
validate and eventually authenticate stream input before allowing RPC dispatch.
See [Untrusted Transport Input](../security/untrusted-transport-input.md).

### Coroutine Shutdown

Coroutine SGX shutdown follows the same reference protocol as other hierarchical
stream transports.

Important rules:

- the enclave must not be terminated merely because the host fixture or
  application released its root pointer
- release messages must be allowed to run inside the enclave so implementation
  destructors can release any host interfaces they hold
- the stream transport and service must stay alive until release call stacks have
  unwound
- refcount-zero shutdown is a clean disconnect
- clean disconnect sends the close handshake, not `transport_down`
- `transport_down` is reserved for real transport failure or unexpected peer
  disappearance
- worker ECALLs should return only after the service, transport, proxies, stubs,
  pass-throughs, and active release work have drained

See [Zone Shutdown Sequence](../protocol/zone_shutdown_sequence.md) for the
detailed sequence and checklist.

### Relationship To `notify_all_destinations_of_disconnect`

`notify_all_destinations_of_disconnect` is the failure cleanup path. It informs
pass-throughs and the local service that destinations reachable through this
transport have become unavailable.

It must not be called for ordinary refcount-zero coroutine SGX shutdown. In that
case the destination graph has drained naturally and the stream close handshake
is sufficient.

If this notification is observed during a clean SGX coroutine teardown, treat it
as a bug in shutdown classification rather than as harmless cleanup.

### Nested Enclave Callback Troubleshooting

One historically fragile coroutine SGX shape is:

```text
host -> enclave B -> host callback -> create enclave C -> call C
```

This path creates nested enclave runtime work while an earlier enclave callback
stack is still active. If a test such as
`remote_type_test/12.check_for_call_enclave_zone` hangs, verify the current live
state before relying on older investigation notes. Useful first checks are:

- confirm the host has sent the add-ref or call message to the expected enclave
  queue
- confirm the target enclave's stream receive coroutine is still being polled
- confirm the scheduler is still making progress while the original callback
  stack is suspended
- confirm the failure is not just missing workers; increasing worker ECALL count
  alone did not explain the historical stall
- check queue and scheduler progress with diagnostics that do not write into
  the same hot SPSC protocol path being investigated

The narrower historical suspect was not generic add-ref contention on one stub:
the host queued a message toward enclave B, but enclave B did not observe it
before timeout. That points at queue instance selection, receive-loop progress,
or re-entrant routing work ahead of queue consumption.

## Host Setup

```cpp
auto enclave_proxy = rpc::enclave_service_proxy::create(
    "enclave_service",
    rpc::destination_zone{enclave_zone_id},
    host_service,
    "enclave.signed.so");  // Enclave binary
```

## Enclave Setup

```cpp
// Inside enclave (illustrative test-style setup)
int marshal_test_init_enclave(
    uint64_t host_zone_id,
    uint64_t host_id,
    uint64_t child_zone_id,
    uint64_t* example_object_id)
{
    auto ret = rpc::child_service::create_child_zone<
        rpc::host_service_proxy,
        yyy::i_host,
        yyy::i_example>(
        "test_enclave",
        rpc::zone{child_zone_id},
        rpc::destination_zone{host_zone_id},
        input_descr,
        output_descr,
        [](const rpc::shared_ptr<yyy::i_host>& host,
            rpc::shared_ptr<yyy::i_example>& new_example,
            const std::shared_ptr<rpc::child_service>& child_service_ptr) -> int
        {
            new_example = rpc::make_shared<example_impl>(
                child_service_ptr, host);
            return rpc::error::OK();
        },
        rpc_server);

    return ret;
}
```

## Enclave Service Proxy

```cpp
class enclave_service_proxy : public rpc::service_proxy
{
public:
    static std::shared_ptr<enclave_service_proxy> create(
        const char* name,
        rpc::destination_zone destination_zone_id,
        std::weak_ptr<rpc::service> service,
        const std::string& enclave_path);
};
```

## Host Service Proxy (for enclave-to-host calls)

```cpp
class host_service_proxy : public rpc::service_proxy
{
public:
    static std::shared_ptr<host_service_proxy> create(
        const char* name,
        rpc::caller_zone caller_zone_id,
        std::weak_ptr<rpc::child_service> service);
};
```

This page is best read together with:

- [Hierarchical Transport Pattern](hierarchical.md)
- [Zone Shutdown Sequence](../protocol/zone_shutdown_sequence.md)
- [SGX Coroutine Transport Plan](../sgx_coroutines.md)
- [Zone Hierarchies](../architecture/07-zone-hierarchies.md)
