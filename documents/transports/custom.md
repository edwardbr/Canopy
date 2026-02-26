<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Custom Transports

Implement your own transport by inheriting from `rpc::transport`.

## Required Overrides

```cpp
class my_transport : public rpc::transport
{
public:
    // Connection establishment
    CORO_TASK(int) inner_connect(connection_settings& input_descr,
                                  interface_descriptor& output_descr) override
    {
        // Perform handshake
        status_ = transport_status::CONNECTED;
        CO_RETURN rpc::error::OK();
    }

    // Accept incoming connection
    CORO_TASK(int) inner_accept() override
    {
        // Accept connection from remote
        CO_RETURN rpc::error::OK();
    }

    // Request-response RPC (outbound - called by base class)
    CORO_TASK(int) outbound_send(uint64_t protocol_version,
                                  rpc::encoding encoding,
                                  uint64_t tag,
                                  rpc::caller_zone caller_zone_id,
                                  rpc::remote_object remote_object_id,
                                  rpc::interface_ordinal interface_id,
                                  rpc::method method_id,
                                  const rpc::span& in_data,
                                  std::vector<char>& out_buf_,
                                  const std::vector<rpc::back_channel_entry>& in_back_channel,
                                  std::vector<rpc::back_channel_entry>& out_back_channel) override
    {
        // Serialize message into envelope
        // Send to remote zone
        // Wait for response
        // Deserialize response
    }

    // Fire-and-forget (outbound)
    CORO_TASK(void) outbound_post(uint64_t protocol_version,
                                   rpc::encoding encoding,
                                   uint64_t tag,
                                   rpc::caller_zone caller_zone_id,
                                   rpc::remote_object remote_object_id,
                                   rpc::interface_ordinal interface_id,
                                   rpc::method method_id,
                                   const rpc::span& in_data,
                                   const std::vector<rpc::back_channel_entry>& in_back_channel) override
    {
        // Serialize and send without waiting for response
    }

    // Interface query (outbound)
    CORO_TASK(int) outbound_try_cast(uint64_t protocol_version,
                                      rpc::caller_zone caller_zone_id,
                                      rpc::remote_object remote_object_id,
                                      rpc::interface_ordinal interface_id,
                                      const std::vector<rpc::back_channel_entry>& in_back_channel,
                                      std::vector<rpc::back_channel_entry>& out_back_channel) override;

    // Reference counting (outbound)
    CORO_TASK(int) outbound_add_ref(uint64_t protocol_version,
                                     rpc::remote_object remote_object_id,
                                     rpc::caller_zone caller_zone_id,
                                     rpc::requesting_zone requesting_zone_id,
                                     rpc::add_ref_options build_out_param_channel,
                                     const std::vector<rpc::back_channel_entry>& in_back_channel,
                                     std::vector<rpc::back_channel_entry>& out_back_channel) override;

    CORO_TASK(int) outbound_release(uint64_t protocol_version,
                                     rpc::remote_object remote_object_id,
                                     rpc::caller_zone caller_zone_id,
                                     rpc::release_options options,
                                     const std::vector<rpc::back_channel_entry>& in_back_channel,
                                     std::vector<rpc::back_channel_entry>& out_back_channel) override;

    // Lifecycle notifications (outbound)
    CORO_TASK(void) outbound_object_released(uint64_t protocol_version,
                                              rpc::remote_object remote_object_id,
                                              rpc::caller_zone caller_zone_id,
                                              const std::vector<rpc::back_channel_entry>& in_back_channel) override;

    CORO_TASK(void) outbound_transport_down(uint64_t protocol_version,
                                             rpc::destination_zone destination_zone_id,
                                             rpc::caller_zone caller_zone_id,
                                             const std::vector<rpc::back_channel_entry>& in_back_channel) override;
};
```

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

If implementing a hierarchical transport (like local, SGX, or DLL) that creates parent/child zone relationships:

1. **Use the standard pattern**: See `documents/transports/hierarchical.md`
2. **Implement circular dependency**: `parent_transport` and `child_transport` reference each other
3. **Stack-based protection**: Use `auto child = child_.get_nullable()` before boundary crossing
4. **Safe disconnection**: Override `set_status()` and implement `on_child_disconnected()`
5. **Thread safety**: Use `stdex::member_ptr` for cross-zone references

Examples:
- **Local**: `transports/local/` - In-process parent/child
- **SGX**: Enclave transport - Host/enclave boundary
- **DLL**: Cross-DLL boundary (platform-specific)
