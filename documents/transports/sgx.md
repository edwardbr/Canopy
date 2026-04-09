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

## When to Use

- Secure computation with hardware guarantees
- Protecting sensitive data processing
- Confidential computing scenarios

## Requirements

- Intel SGX SDK
- `CANOPY_BUILD_ENCLAVE=ON`

## Architecture

The SGX path is a C++ hierarchical transport family. It should be read as a
specialised host/enclave boundary implementation, not as a universal Canopy
transport capability.

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

- [documents/transports/hierarchical.md](/var/home/edward/projects/Canopy/documents/transports/hierarchical.md)
- [documents/architecture/07-zone-hierarchies.md](/var/home/edward/projects/Canopy/documents/architecture/07-zone-hierarchies.md)
