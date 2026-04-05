<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Custom Transports

Implement your own transport by inheriting from `rpc::transport`.

## See Also

- [Stream Backpressure Guidelines](stream_backpressure_guidelines.md)
- [TCP Transport](tcp.md)
- [Hierarchical Transport Pattern](hierarchical.md)

## Required Overrides

```cpp
class my_transport : public rpc::transport
{
public:
    CORO_TASK(rpc::connect_result)
    inner_connect(std::shared_ptr<rpc::object_stub> stub, rpc::connection_settings input_descr) override
    {
        // Perform handshake and return the remote root descriptor
        CO_RETURN rpc::connect_result{rpc::error::OK(), remote_descriptor};
    }

    CORO_TASK(int) inner_accept() override
    {
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(rpc::send_result) outbound_send(rpc::send_params params) override;
    CORO_TASK(void) outbound_post(rpc::post_params params) override;
    CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override;
    CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override;
    CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override;
    CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override;
    CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override;
};
```

The parameter-object types (`send_params`, `post_params`, `try_cast_params`,
and so on) live in `c++/rpc/include/rpc/internal/marshaller.h`. The canonical
virtual interface lives in `c++/rpc/include/rpc/internal/transport.h`.

## Lifecycle Notifications

The base `transport` class handles inbound message processing automatically. Your derived class only needs to implement the `outbound_*` methods for sending messages to the remote zone. Inbound messages are processed by the base class which routes them to:
- Local service (if destination matches zone_id_)
- Passthrough handler (if routing to non-adjacent zone)

**Inbound methods** (implemented by base class, called by your transport):
- `inbound_send()` - Process incoming request-response calls
- `inbound_post()` - Process incoming fire-and-forget notifications
- `inbound_try_cast()` - Process incoming interface queries
- `inbound_add_ref()` - Process incoming reference count increments
- `inbound_release()` - Process incoming reference count decrements
- `inbound_object_released()` - Process incoming object release notifications
- `inbound_transport_down()` - Process incoming transport disconnection

**Outbound methods** (override in your derived class):
- `outbound_send()` - Send request-response calls to remote
- `outbound_post()` - Send fire-and-forget notifications to remote
- `outbound_try_cast()` - Send interface queries to remote
- `outbound_add_ref()` - Send reference count increments to remote
- `outbound_release()` - Send reference count decrements to remote
- `outbound_object_released()` - Send object release notifications to remote
- `outbound_transport_down()` - Send transport disconnection to remote

## Hierarchical Transports (Parent/Child Zones)

If implementing a hierarchical transport (like local, SGX, or the in-process
DLL transports) that creates parent/child zone relationships:

1. **Use the standard pattern**: See `documents/transports/hierarchical.md`
2. **Implement circular dependency**: `parent_transport` and `child_transport` reference each other
3. **Stack-based protection**: Use `auto child = child_.get_nullable()` before boundary crossing
4. **Safe disconnection**: Override `set_status()` and implement `on_child_disconnected()`
5. **Thread safety**: Use `stdex::member_ptr` for cross-zone references

Examples:
- **Local**: `c++/transports/local/` - In-process parent/child
- **SGX**: Enclave transport - Host/enclave boundary
- **DLL**: In-process shared-library child zone
