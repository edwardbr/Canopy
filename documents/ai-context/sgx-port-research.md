# SGX Port Research Notes

Session: 2026-04-10. Goal: get `Debug_SGX_Sim` building, starting with `c++/tests/test_enclave`.

## The Core Problem

The SGX transport code was written against the **old** Canopy API where `service_proxy` had
virtual `send`, `post`, `try_cast`, `add_ref`, `release` methods. After the coroutine
refactoring, that API is gone. Transport implementations now inherit from `rpc::transport`
and override `outbound_*` methods. Everything in `c++/transports/sgx/src/` is stale.

## Old API (gone)

`service_proxy` had pure virtual methods: `send(...)`, `post(...)`, `try_cast(...)`,
`add_ref(...)`, `release(...)`. `enclave_service_proxy` and `host_service_proxy` extended
`service_proxy` and implemented those directly.

## New API (current)

`rpc::transport` (in `c++/rpc/include/rpc/internal/transport.h`) inherits `i_marshaller`
and defines:

- Final public methods: `send`, `post`, `try_cast`, `add_ref`, `release`, `object_released`,
  `transport_down`, `get_new_zone_id` — these call `outbound_*` + do bookkeeping.
- Pure virtual methods to implement: `inner_connect`, `inner_accept`, `outbound_send`,
  `outbound_post`, `outbound_try_cast`, `outbound_add_ref`, `outbound_release`,
  `outbound_object_released`, `outbound_transport_down`.

Parameter bundles (`send_params`, `post_params`, etc.) live in
`c++/rpc/include/rpc/internal/marshaller_params.h`.

## Canonical Pattern: DLL Transport

The DLL transport (`c++/transports/dynamic_library/`) is the closest analogue to SGX and is
fully updated to the new API. Study it as the reference implementation.

- `child_transport` (host side, `transport.cpp`) — calls into DLL via function pointers.
  `outbound_*` methods invoke `dll_send_(dll_ctx_, &params, &result)` etc.
- `parent_transport` (DLL side, `dll_transport.cpp`) — calls back to host via function
  pointers. `outbound_*` methods invoke `host_send_(host_ctx_, &params, &result)` etc.

For SGX, replace function pointers with ECALLs/OCALLs.

## New Operations (not yet in EDL)

The new `i_marshaller` has two operations the old EDL doesn't cover:
- `object_released` — notifies that a remote object was destroyed (fire-and-forget)
- `transport_down` — notifies that a transport is going down (fire-and-forget)

`get_new_zone_id` is needed if an enclave ever creates sub-zones (defer unless needed).

## OCALL Routing Problem

SGX OCALLs are global functions (no `this`). When the enclave calls `call_host(...)` OCALL,
the host implementation needs to route to the right `enclave_transport` instance.

**Solution**: thread-local storage. Before each ECALL, `enclave_transport::outbound_send`
stores `this` in a thread-local. The global OCALL implementations read from that thread-local.
Works because OCALLs execute synchronously on the same thread as the originating ECALL.

## SGX SDK Status

The submodule at `submodules/confidential-computing.sgx/` is **source only** — no built
binaries. Needs either:
1. `make sdk` within the submodule (requires OCaml for `sgx_edger8r`, Python, build tools)
2. Intel pre-built SDK installer downloaded and installed to a path inside the repo

Headers are at `submodules/confidential-computing.sgx/common/inc/`.
`sgx_edger8r` source is at `submodules/confidential-computing.sgx/sdk/edger8r/linux/` (OCaml).
Simulation libs need to be built from `submodules/confidential-computing.sgx/sdk/simulation/`.

CMakePresets.json Base preset has `"SGX_DIR:FILEPATH": "/home/edward/sgx/sgxsdk"` — needs
updating to wherever SDK gets installed.

## Files That Need Changing

### New files to create
- `c++/transports/sgx/src/enclave_transport.h` — host-side transport (replaces `enclave_service_proxy.h`)
- `c++/transports/sgx/src/enclave_transport.cpp` — host-side outbound_* call ECALLs
- `c++/transports/sgx/src/host_transport.h` — enclave-side transport (replaces `host_service_proxy.h`)
- `c++/transports/sgx/src/host_transport.cpp` — enclave-side outbound_* call OCALLs
- `c++/transports/sgx/src/ocall_router.h` — thread-local OCALL routing helpers (host side)

### Files to update
- `c++/transports/sgx/edl/enclave_marshal_test.edl` — add `object_released_*`, `transport_down_*`
- `c++/transports/sgx/CMakeLists.txt` — new target names, remove old proxy targets
- `c++/tests/test_enclave/marshal_test_enclave.cpp` — update ECALL bodies + init to new API
- `c++/transports/sgx/tests/transport/tests/sgx/setup.h` — update host setup to new API
- `cmake/FindSGX.cmake` — point to submodule or installed location
- `CMakePresets.json` — update `SGX_DIR` to correct path

### Files to delete
- `c++/transports/sgx/src/enclave_service_proxy.cpp` (replaced)
- `c++/transports/sgx/src/enclave_service_proxy.h` (replaced)
- `c++/transports/sgx/src/host_service_proxy.cpp` (replaced)
- `c++/transports/sgx/src/host_service_proxy.h` (replaced)

## API: marshal_test_enclave.cpp Changes

Old init:
```cpp
rpc::child_service::create_child_zone<rpc::host_service_proxy, i_host, i_example>(
    "test_enclave", zone, dest_zone, input_descr, output_descr, lambda, rpc_server);
```

New init — create a `host_transport` and pass it:
```cpp
auto parent_t = std::make_shared<rpc::sgx::host_transport>("test_enclave", child_zone_id, host_zone_id);
auto result = child_service::create_child_zone<i_host, i_example>(
    "test_enclave", parent_t, input_descr, lambda);
```

Old ECALL body: `rpc_server->send(protocol_version, ...)` 
New ECALL body: `parent_transport_->inbound_send(send_params{...})`

## API: setup.h (host side) Changes

Old:
```cpp
root_service_->connect_to_zone<rpc::enclave_service_proxy>(
    "main child", {new_zone_id}, i_host_ptr_, i_example_ptr_, enclave_path);
```

New:
```cpp
auto child_t = std::make_shared<rpc::sgx::enclave_transport>("main child", root_service_, enclave_path);
auto result = CO_AWAIT root_service_->connect_to_zone<i_host, i_example>(
    "main child", child_t, i_host_ptr_);
```

## Key Reference Files

- `c++/transports/dynamic_library/src/transport.cpp` — host-side hierarchical transport pattern
- `c++/transports/dynamic_library/src/dll_transport.cpp` — child-side hierarchical transport pattern
- `c++/transports/local/src/transport.cpp` — simpler hierarchical example
- `documents/transports/hierarchical.md` — lifecycle and circular-ref protocol
- `documents/transports/custom.md` — full list of virtuals to override
- `c++/rpc/include/rpc/internal/transport.h` — base class definition
- `c++/rpc/include/rpc/internal/marshaller_params.h` — all param/result structs
