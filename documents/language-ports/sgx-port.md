# SGX Transport Port Plan

Goal: get `cmake --preset Debug_SGX_Sim` building and `marshal_test_enclave` running on a
non-Intel (simulation) machine, with full correctness on real SGX hardware deferred to a
later session on an Intel machine.

## Background

The SGX transport existed in the old `edwardbr/rpc` repository. During the Canopy refactor
(adding coroutines, renaming everything), the transport API changed fundamentally:

- **Old**: `service_proxy` had virtual `send`/`post`/`try_cast`/`add_ref`/`release`.
  `enclave_service_proxy` and `host_service_proxy` overrode these directly.
- **New**: `rpc::transport` inherits `i_marshaller` and exposes `outbound_*` virtual methods.
  Derived classes implement `outbound_send`, `outbound_post`, etc. The base class handles
  routing, ref-counting bookkeeping, and passthrough management.

Additionally, `i_marshaller` gained three operations the old EDL does not cover:
`object_released`, `transport_down`, and `get_new_zone_id`.

All SGX proxy code is stale. It will not compile, and even if it did, it would bypass the
transport layer's ref-count and passthrough machinery.

## Compiling `./rpc` Against SGX — Challenges and Status

The `c++/rpc/CMakeLists.txt` already defines an `rpc_enclave` target that compiles all of
`src/*.cpp` with enclave settings (`CANOPY_ENCLAVE_DEFINES`, `CANOPY_ENCLAVE_COMPILE_OPTIONS`,
SGX libc/libc++ include paths, `fmt::fmt-header-only`). So the build wiring for the core
library exists. The remaining challenges are:

### 1. SGX SDK must be installed first

`FindSGX.cmake` sets `SGX_INCLUDE_DIR`, `SGX_TLIBC_INCLUDE_DIR`, `SGX_LIBCXX_INCLUDE_DIR`,
and `SGX_EDGER8R` from the installed SDK at `SGX_DIR`. Until the SDK is built from
`submodules/confidential-computing.sgx/` and installed, `cmake --preset Debug_SGX_Sim` will
fail at `find_package(SGX REQUIRED)` before any source is touched.

### 2. `std::shared_mutex` in `transport.h`

`transport.h` includes `<shared_mutex>`. Intel's trusted libc++ (`libstdc++` inside the
enclave) may not provide `std::shared_mutex` in older SDK versions. If the header is missing,
every translation unit that includes `transport.h` will fail to compile under
`-nostdinc++`. Resolution options:
- Replace with a plain `std::mutex` inside the enclave (guarded by `#ifdef FOR_SGX`).
- Verify the SDK version includes it — the `libcoro_for_enclaves` submodule suggests recent
  libc++ is in use and may have it.

### 3. `CORO_TASK` / coroutine macros

SGX builds are non-coroutine (`CANOPY_BUILD_COROUTINE=OFF`). `CORO_TASK(T)` collapses to a
plain return type and `CO_RETURN`/`CO_AWAIT` become simple `return`/call-through. This is
already handled correctly by the macros; no code change needed.

### 4. `<fmt/format.h>` vs `<fmt/format-inl.h>`

The `FOR_SGX` guard selects `fmt::fmt-header-only` (the header-only variant) instead of
the compiled fmt library. This is already wired in `SGX.cmake` (`CANOPY_ENCLAVE_FMT_LIB`).
Verify that every `#include <fmt/format.h>` in `src/*.cpp` is either absent or already
guarded.

### 5. CMakePresets.json `SGX_DIR`

The base preset has `"SGX_DIR:FILEPATH": "/home/edward/sgx/sgxsdk"`. This path must exist
before `cmake --preset Debug_SGX_Sim`. CI uses `/opt/intel/sgxsdk` (the `FindSGX.cmake`
default fallback).

### Summary of steps to get `rpc_enclave` compiling

1. Build and install the SGX SDK (Phase 0 below).
2. Run `cmake --preset Debug_SGX_Sim`.
3. Run `cmake --build build_debug_sgx_sim --target rpc_enclave 2>&1 | head -60`.
4. Fix any `std::shared_mutex` or fmt include errors by adding `#ifdef FOR_SGX` guards.
5. Only once `rpc_enclave` compiles cleanly, proceed to the transport and enclave layers.

---

## Canonical Reference: DLL Transport

The dynamic-library transport (`c++/transports/dynamic_library/`) is the closest analogue to
SGX and is fully updated. Use it as the implementation template throughout this plan.

- `c++/transports/dynamic_library/src/transport.cpp` — host-side (`child_transport`)
- `c++/transports/dynamic_library/src/dll_transport.cpp` — child-side (`parent_transport`)

The SGX transport differs only in the boundary mechanism: function pointers become
ECALLs/OCALLs, and parameters must be serialized (YAS binary) because they cross a trust
boundary.

## OCALL Routing

SGX OCALLs are global C functions with no `this` pointer. When the enclave calls back to the
host (e.g. `call_host(...)`), the OCALL implementation routes via the **singleton host
service** (`current_host_service`, a `std::weak_ptr<rpc::service>` in `test_globals.h`):

```cpp
// In the OCALL shim (host side, global C linkage):
extern "C" int call_host(...) {
    auto svc = current_host_service.lock();
    auto transport = svc->get_transport(destination_zone{destination_zone_id});
    auto result = transport->inbound_send(send_params{...});
    // serialize result back into out buffers
    return result.error_code;
}
```

All i_marshaller-related OCALLs route this way. Non-marshaller OCALLs (e.g. `rpc_log`,
`hang`) call their own static functions directly.

`service::get_transport(destination_zone)` is defined in `c++/rpc/src/service.cpp`.

## `get_new_zone_id` in EDL

`get_new_zone_id` is now part of the `i_marshaller` interface and is serviced by the
`rpc::service`. It must be added as an OCALL so the enclave can request new zone IDs from
the host's root service when creating sub-zones.

## Test Runner

Tests are run via the existing `rpc_test` binary. When `CANOPY_BUILD_ENCLAVE=ON`:
- `c++/tests/test_host/CMakeLists.txt` already links `${SGX_TRANSPORT_TEST_LIB}` and
  `${ENCLAVE_DEPENDANCIES}`
- The signed enclave `.so` path is passed via `enclave_path` global (set in
  `test_globals.cpp`)
- Run: `./rpc_test --gtest_filter="*Sgx*"`

---

## Phase 0 — Prerequisites

### On your local Fedora 43 machine

The SGX SDK must be built from the submodule source (`submodules/confidential-computing.sgx`).
Install the build prerequisites:

```bash
sudo dnf install ocaml ocaml-ocamlbuild autoconf automake libtool
```

(`gcc`, `gcc-c++`, `openssl-devel`, `python3`, `perl`, `cmake`, `git` are assumed already
present from the normal Canopy build environment.)

Then build the SDK and install it:

```bash
cd /var/home/edward/projects/Canopy_gitlab/submodules/confidential-computing.sgx
make sdk_no_mitigation SGX_DEBUG=1
# Produces an installer at linux/installer/bin/sgx_linux_x64_sdk_*.bin
./linux/installer/bin/sgx_linux_x64_sdk_*.bin --prefix=/home/edward/sgx
# Creates /home/edward/sgx/sgxsdk/
```

`CMakePresets.json` Base preset already has `"SGX_DIR:FILEPATH": "/home/edward/sgx/sgxsdk"`,
so no preset change is needed for local builds.

Verify the SDK was found:
```bash
cmake --preset Debug_SGX_Sim 2>&1 | grep -E "SGX_FOUND|edger8r|sgxsdk"
```

### In CI

The workflow builds the SDK inside the runner and installs to `/opt/intel/sgxsdk`
(the `FindSGX.cmake` default), so no `SGX_DIR` override is needed in CI.
See the `build-sgx-sim` job in `.github/workflows/main.yml`.

---

## Phase 1 — Transport Rewrite

Replace the stale `service_proxy` subclasses with proper `rpc::transport` subclasses.

### 1a. Delete stale files

```
c++/transports/sgx/src/enclave_service_proxy.cpp  → DELETE
c++/transports/sgx/src/enclave_service_proxy.h    → DELETE
c++/transports/sgx/src/host_service_proxy.cpp     → DELETE
c++/transports/sgx/src/host_service_proxy.h       → DELETE
```

### 1b. Create `rpc::sgx::enclave_transport` (host side)

File: `c++/transports/sgx/src/enclave_transport.h`

```cpp
namespace rpc::sgx
{
    // Lives in the host zone. Calls into the enclave via ECALLs.
    // Mirrors rpc::dynamic_library::child_transport.
    class enclave_transport : public rpc::transport
    {
    public:
        enclave_transport(std::string name,
                          std::shared_ptr<rpc::service> service,
                          std::string enclave_path);
        ~enclave_transport() override;

    protected:
        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub> stub,
                      rpc::connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(rpc::send_result)     outbound_send(rpc::send_params params) override;
        CORO_TASK(void)                 outbound_post(rpc::post_params params) override;
        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override;
        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override;
        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override;
        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override;
        CORO_TASK(rpc::new_zone_id_result) outbound_get_new_zone_id(
            rpc::get_new_zone_id_params params) override;  // not needed — base routes to service

    private:
        std::string enclave_path_;
        sgx_enclave_id_t eid_ = 0;
        // Enclave owner RAII: destroys enclave on destruct
        struct enclave_owner { sgx_enclave_id_t eid; ~enclave_owner(); };
        std::shared_ptr<enclave_owner> owner_;
    };
}
```

`inner_connect` implementation:
1. Allocate a zone ID via the parent service `get_new_zone_id`.
2. Set adjacent zone ID, register with service via `svc->add_transport`.
3. Call `sgx_create_enclave` (simulation mode: `SGX_DEBUG_FLAG=1`).
4. Call `marshal_test_init_enclave` ECALL, passing host zone, input object, enclave zone.
5. Receive back enclave's root object ID.
6. Set status `CONNECTED`, return `connect_result`.

Each `outbound_*` method:
1. Serializes params to YAS binary (combined payload+back-channel).
2. Calls the corresponding ECALL (e.g. `::call_enclave(eid_, &err_code, ...)`).
3. Handles `NEED_MORE_MEMORY` retry by resizing buffer.
4. Deserializes result and returns it.

`outbound_get_new_zone_id` does NOT need to be overridden — the base `transport`
implementation forwards to the local service.

### 1c. Create `rpc::sgx::host_transport` (enclave side)

File: `c++/transports/sgx/src/host_transport.h`

```cpp
namespace rpc::sgx
{
    // Lives inside the enclave zone. Calls back to host via OCALLs.
    // Mirrors rpc::dynamic_library::parent_transport.
    class host_transport : public rpc::transport
    {
    public:
        host_transport(std::string name,
                       rpc::zone enclave_zone,
                       rpc::zone host_zone);

    protected:
        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub>,
                      rpc::connection_settings) override { CO_RETURN rpc::connect_result{}; }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(rpc::send_result)     outbound_send(rpc::send_params params) override;
        CORO_TASK(void)                 outbound_post(rpc::post_params params) override;
        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override;
        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override;
        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override;
        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override;
        CORO_TASK(rpc::new_zone_id_result) outbound_get_new_zone_id(
            rpc::get_new_zone_id_params params) override;  // calls get_new_zone_id_host OCALL
    };
}
```

`outbound_*` methods serialize params, call the OCALL, deserialize result.
`outbound_get_new_zone_id` calls `get_new_zone_id_host(...)` OCALL which routes to the
host's root service zone allocator.

`set_status(DISCONNECTED)` override (like `dll_transport`):
```cpp
void host_transport::set_status(rpc::transport_status status) {
    rpc::transport::set_status(status);
    if (status == rpc::transport_status::DISCONNECTED)
        notify_all_destinations_of_disconnect();
}
```

---

## Phase 2 — EDL Updates

File: `c++/transports/sgx/edl/enclave_marshal_test.edl`

Add to `trusted` (ECALLs):
```edl
public int object_released_enclave(
    uint64_t protocol_version,
    uint64_t object_id,
    uint64_t caller_zone_id,
    size_t sz_in_back_channel,
    [in, size=sz_in_back_channel] const char* in_back_channel
);

public int transport_down_enclave(
    uint64_t protocol_version,
    uint64_t destination_zone_id,
    uint64_t caller_zone_id,
    size_t sz_in_back_channel,
    [in, size=sz_in_back_channel] const char* in_back_channel
);

public int get_new_zone_id_enclave(
    uint64_t protocol_version,
    size_t sz_in_back_channel,
    [in, size=sz_in_back_channel] const char* in_back_channel,
    size_t sz_out_back_channel,
    [out, size=sz_out_back_channel] char* out_back_channel,
    [out] size_t* out_back_channel_sz,
    [out] uint64_t* new_zone_id
);
```

Add to `untrusted` (OCALLs):
```edl
int object_released_host(
    uint64_t protocol_version,
    uint64_t object_id,
    uint64_t caller_zone_id,
    size_t sz_in_back_channel,
    [in, size=sz_in_back_channel] const char* in_back_channel
);

int transport_down_host(
    uint64_t protocol_version,
    uint64_t destination_zone_id,
    uint64_t caller_zone_id,
    size_t sz_in_back_channel,
    [in, size=sz_in_back_channel] const char* in_back_channel
);

int get_new_zone_id_host(
    uint64_t protocol_version,
    size_t sz_in_back_channel,
    [in, size=sz_in_back_channel] const char* in_back_channel,
    size_t sz_out_back_channel,
    [out, size=sz_out_back_channel] char* out_back_channel,
    [out] size_t* out_back_channel_sz,
    [out] uint64_t* new_zone_id
);
```

The existing ECALLs (`call_enclave`, `post_enclave`, `try_cast_enclave`, `add_ref_enclave`,
`release_enclave`) and their OCALL mirrors are retained with minor serialization adjustments.

---

## Phase 3 — `marshal_test_enclave.cpp` Updates

### Global state

```cpp
// Global transport — set during marshal_test_init_enclave, used by ECALL bodies
static std::shared_ptr<rpc::sgx::host_transport> g_host_transport;
static std::shared_ptr<rpc::child_service> rpc_server;
```

### `marshal_test_init_enclave`

Old pattern (service_proxy-based, stale):
```cpp
rpc::child_service::create_child_zone<rpc::host_service_proxy, i_host, i_example>(
    "test_enclave", zone, dest_zone, input_descr, output_descr, lambda, rpc_server);
```

New pattern (transport-based):
```cpp
int marshal_test_init_enclave(uint64_t host_zone_id, uint64_t host_id,
                               uint64_t child_zone_id, uint64_t* example_object_id)
{
    rpc::connection_settings input_descr{{host_id}, {host_zone_id}};

    g_host_transport = std::make_shared<rpc::sgx::host_transport>(
        "test_enclave", rpc::zone{child_zone_id}, rpc::zone{host_zone_id});

    auto result = child_service::create_child_zone<yyy::i_host, yyy::i_example>(
        "test_enclave",
        g_host_transport,
        input_descr,
        [](const rpc::shared_ptr<yyy::i_host>& host,
           std::shared_ptr<rpc::child_service> child_svc)
        {
            rpc_server = child_svc;
            rpc::shared_ptr<yyy::i_example> new_example(
                new marshalled_tests::example(child_svc, host));
            return service_connect_result<yyy::i_example>{rpc::error::OK(), new_example};
        });

    if (result.error_code != rpc::error::OK())
        return result.error_code;

    *example_object_id = result.output_object.object_id.get_val();
    return rpc::error::OK();
}
```

### ECALL bodies

Old: `rpc_server->send(...)` (flat args, old service API)

New: deserialize to params struct, route through transport:
```cpp
int call_enclave(...) {
    // ... deserialize combined_in into payload + in_back_channel ...
    send_params params{protocol_version, rpc::encoding(encoding), tag,
                       {caller_zone_id}, {{zone_id}, {object_id}},
                       {interface_id}, {method_id},
                       std::move(payload), std::move(in_back_channel)};

    auto result = g_host_transport->inbound_send(std::move(params));
    // ... serialize result back, handle NEED_MORE_MEMORY ...
    return result.error_code;
}
```

Same pattern for `post_enclave` → `inbound_post`, `try_cast_enclave` → `inbound_try_cast`,
`add_ref_enclave` → `inbound_add_ref`, `release_enclave` → `inbound_release`.

New ECALL bodies for `object_released_enclave`, `transport_down_enclave`:
```cpp
int object_released_enclave(...) {
    // deserialize back channel
    object_released_params params{...};
    g_host_transport->inbound_object_released(std::move(params));
    return rpc::error::OK();
}
```

New ECALL body for `get_new_zone_id_enclave`:
```cpp
int get_new_zone_id_enclave(...) {
    // The enclave's child_service handles this locally — route to rpc_server
    get_new_zone_id_params params{protocol_version, std::move(in_back_channel)};
    auto result = rpc_server->get_new_zone_id(std::move(params));
    // serialize result...
    *new_zone_id = result.zone_id.get_subnet();
    return result.error_code;
}
```

---

## Phase 4 — Host OCALL Implementations

OCALLs live in a new file: `c++/transports/sgx/src/ocall_impls.cpp` (host side, non-enclave).

Each OCALL routes through the singleton service using `current_host_service`:

```cpp
extern "C" int call_host(uint64_t protocol_version, uint64_t encoding, uint64_t tag,
    uint64_t caller_zone_id, uint64_t destination_zone_id,
    uint64_t object_id, uint64_t interface_id, uint64_t method_id,
    size_t sz_in, const char* data_in,
    size_t sz_out, char* data_out, size_t* data_out_sz)
{
    auto svc = current_host_service.lock();
    if (!svc) return rpc::error::ZONE_NOT_FOUND();

    // Deserialize combined_in into payload + in_back_channel
    ...

    send_params params{protocol_version, rpc::encoding(encoding), tag,
                       {caller_zone_id}, {{destination_zone_id}, {object_id}},
                       {interface_id}, {method_id},
                       std::move(payload), std::move(in_back_channel)};

    auto transport = svc->get_transport(rpc::destination_zone{destination_zone_id});
    if (!transport) return rpc::error::ZONE_NOT_FOUND();

    auto result = transport->inbound_send(std::move(params));
    // Serialize result, handle NEED_MORE_MEMORY...
    *data_out_sz = ...;
    return result.error_code;
}
```

Same pattern for `post_host`, `try_cast_host`, `add_ref_host`, `release_host`,
`object_released_host`, `transport_down_host`.

`get_new_zone_id_host` routes to the root service's zone allocator:
```cpp
extern "C" int get_new_zone_id_host(...) {
    auto svc = current_host_service.lock();
    get_new_zone_id_params params{...};
    auto result = svc->get_new_zone_id(std::move(params));
    *new_zone_id = result.zone_id.get_subnet();
    return result.error_code;
}
```

`rpc_log` and `hang` remain as direct static function calls (no service routing).

---

## Phase 5 — Host Test Setup Updates

File: `c++/transports/sgx/tests/transport/tests/sgx/setup.h`

Old (stale):
```cpp
auto err_code = root_service_->connect_to_zone<rpc::enclave_service_proxy>(
    "main child", {new_zone_id}, use_host_in_child_ ? i_host_ptr_ : nullptr,
    i_example_ptr_, enclave_path);
```

New:
```cpp
auto child_t = std::make_shared<rpc::sgx::enclave_transport>(
    "main child", root_service_, enclave_path);

auto result = root_service_->connect_to_zone<yyy::i_host, yyy::i_example>(
    "main child", child_t, use_host_in_child_ ? i_host_ptr_ : nullptr);

RPC_ASSERT(result.error_code == rpc::error::OK());
i_example_ptr_ = result.output_interface;
```

---

## Phase 6 — CMake Updates

### `c++/transports/sgx/CMakeLists.txt`

Replace old proxy library targets:

```cmake
if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(edl)

  # Enclave-side transport (compiled FOR_SGX)
  add_library(transport_sgx_enclave src/host_transport.cpp src/host_transport.h)
  target_compile_definitions(transport_sgx_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(transport_sgx_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_include_directories(transport_sgx_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(transport_sgx_enclave PRIVATE rpc::rpc_enclave yas_common)
  target_link_options(transport_sgx_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET transport_sgx_enclave PROPERTY COMPILE_PDB_NAME transport_sgx_enclave)

  # Host-side transport + OCALL implementations
  add_library(transport_sgx
    src/enclave_transport.cpp src/enclave_transport.h
    src/ocall_impls.cpp)
  target_compile_definitions(transport_sgx PRIVATE ${CANOPY_DEFINES})
  target_include_directories(transport_sgx
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE ${CANOPY_INCLUDES})
  target_link_libraries(transport_sgx PRIVATE rpc::rpc yas_common ${CANOPY_LIBRARIES})
  target_link_directories(transport_sgx PUBLIC ${SGX_LIBRARY_PATH})
  target_compile_options(transport_sgx PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_sgx PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET transport_sgx PROPERTY COMPILE_PDB_NAME transport_sgx)

  add_library(transport_sgx_test INTERFACE)
  target_include_directories(transport_sgx_test INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
endif()
```

### `c++/tests/CMakeLists.txt`

Add (if not already present):
```cmake
if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(test_enclave)
endif()
```

### `c++/tests/test_enclave/CMakeLists.txt`

Update `TRUSTED_LIBS`:
```cmake
add_enclave_library(
  marshal_test_enclave
  SRCS marshal_test_enclave.cpp
  TRUSTED_LIBS
    rpc_enclave
    marshal_test_edl_enclave
    transport_sgx_enclave          # replaces old rpc_sgx_enclave_service_proxy
    ${CANOPY_ENCLAVE_TELEMETRY}
    ${CANOPY_TELEMETRY_ENCLAVE_EDL}
    common_enclave
    example_import_idl_enclave
    example_idl_enclave
    example_shared_idl_enclave
    yas_common
    ${SGX_ENCLAVE_LIBS})
```

### `c++/tests/test_host/CMakeLists.txt`

When `CANOPY_BUILD_ENCLAVE`, add the enclave binary as a test dependency and define its path:
```cmake
if(CANOPY_BUILD_ENCLAVE)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_ENCLAVE_PATH="$<TARGET_FILE:marshal_test_enclave.signed>")
  add_dependencies(rpc_test marshal_test_enclave.signed)
endif()
```

---

## Phase 7 — Build and Fix

```bash
cmake --preset Debug_SGX_Sim
cmake --build build_debug_sgx_sim --target marshal_test_enclave 2>&1 | head -80
cmake --build build_debug_sgx_sim --target rpc_test 2>&1 | head -80
```

Iterate on compile errors. Common expected issues:
- Missing includes (enclave headers under `#ifdef FOR_SGX`)
- `create_child_zone` template parameter count or lambda signature mismatch
- YAS format-inl include guard (`FOR_SGX` selects `fmt/format-inl.h`)
- Generated EDL header paths (`build_debug_sgx_sim/generated/`)

---

## Phase 8 — Test

```bash
./build_debug_sgx_sim/output/rpc_test --gtest_filter="*Sgx*" --telemetry-console
```

In simulation mode (`SGX_HW=OFF`), the enclave runs without hardware SGX. All functional
tests should pass. Performance and attestation tests are deferred to a real Intel machine.

---

## Deferred (real Intel machine)

- Hardware SGX mode (`SGX_HW=ON`, `SGX_MODE=release`)
- DCAP attestation integration
- Performance measurement

---

## Notes

- `CORO_TASK` / `CO_AWAIT` / `CO_RETURN` macros collapse to synchronous equivalents in
  non-coroutine builds. The SGX transport is non-coroutine only (enclaves don't support
  coroutines). `CORO_TASK(send_result) outbound_send(...)` expands to a plain function.

- In enclave builds, include `<fmt/format-inl.h>` (not `<fmt/format.h>`) when `FOR_SGX`
  is defined. Pattern is in existing `c++/rpc/src/service.cpp`.

- YAS serialization is available in both host and enclave builds via `yas_common`.

- `SGX_DEBUG_FLAG` is `1` in debug/simulation builds and `0` in release; set by
  `FindSGX.cmake` based on `SGX_MODE`.

- `common_enclave` target provides `common/foo_impl.h`, `common/host_service_proxy.h`,
  etc. to the enclave build. After the rewrite, it will need `host_transport.h` instead.
