<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Refactoring Zone Addressing to IPv4/IPv6-Style

## Context

Canopy identifies zones using a simple `uint64_t` counter, which works for single-process testing but doesn't support network-routable addressing or hierarchical subnet-based allocation. The goal is to refactor zone identity to use an IPv4/IPv6-inspired addressing model with structured fields for routing prefix, subnet (zone), and object identity. This enables future TUN-based transport integration, deterministic routing based on address structure, and merging of `destination_zone` + `object_id` into a single address in `i_marshaller`.

The IDL remains the golden definition for all types. Transport wire protocol IDLs (tcp.idl, spsc.idl) will use the typed structs from `rpc_types.idl` instead of raw `uint64_t` fields. The dodgy transport is being removed.

## Current State (Post Phase 1)

**Zone types** in `rpc/interfaces/rpc/rpc_types.idl`: `zone`, `destination_zone`, `caller_zone`, `requesting_zone` each wrap a `zone_address` struct internally. The `zone_address` struct currently has fixed fields: `uint64_t routing_prefix`, `uint32_t subnet_id`, `uint32_t object_id`.

**i_marshaller** (`rpc/include/rpc/internal/marshaller.h`): Every method takes separate `destination_zone` and `object` parameters. Verified that `destination_zone_id + object_id` always identifies "which object at which zone" across ALL methods (including `object_released` where the message travels in the reverse direction).

**Wire protocols** (`transports/tcp/interface/tcp/tcp.idl`, `transports/spsc/interface/spsc/spsc.idl`): Use raw `uint64_t` fields with manual `.get_subnet()` extraction in transport implementations. Already `#import "rpc/rpc_types.idl"`.

**Zone generation**: Global `std::atomic<uint64_t>` counter in `service.cpp:32-37`.

**Hash/equality**: `std::hash` specializations in `rpc/include/rpc/internal/types.h` delegate to `zone_address` hash.

## Design

### 1. Configurable `zone_address` Type

The `zone_address` struct supports three addressing modes, selectable at build time via CMake:

**Mode: None (local-only)**
No routing prefix. The address is a simple local identifier, equivalent to today's behaviour. Subnet identifies the zone, object identifies the resource within it. Suitable for single-process and in-memory transports.

**Mode: IPv4 with local suffix**
A 32-bit IPv4 address serves as the routing prefix. The remaining address space is split between subnet and object fields. Suitable for traditional network deployments where IPv4 addressing is used between nodes.

**Mode: IPv6**
Full 128-bit address space. The routing prefix occupies the upper bits, with subnet and object sharing the remaining space. The object ID can either be embedded within the 128-bit IPv6 address or stored in its own separate 64-bit field. This choice determines whether the address fits in a single IPv6 address or requires an extension.

### Field Width Configuration

The subnet and object field widths are configurable independently:

- **Subnet size**: 0 to 64 bits (configured via CMake)
- **Object size**: 0 to 64 bits (configured via CMake)

This allows the same `zone_address` type to serve different deployment scenarios:
- Embedded systems: small subnet (8-16 bits), no object field
- Single-process testing: medium subnet (32 bits), no routing prefix
- Network deployment: full routing prefix with appropriately sized subnet and object fields

### IDL Struct Definition

The `zone_address` fields are **private** in the IDL, with public getter/setter methods as the only interface. This decouples consuming code from the internal layout, allowing `#ifdef` blocks in the private section to define different struct layouts for different address modes at compile time.

**Public API** (stable across all modes):
```cpp
uint64_t get_routing_prefix() const;
void set_routing_prefix(uint64_t val);
uint64_t get_subnet() const;
void set_subnet(uint64_t val);
uint64_t get_object_id() const;
void set_object_id(uint64_t val);
```

**IDL definition with configurable private layout:**
```idl
struct zone_address
{
private:
#cpp_quote(R^__(
#if defined(CANOPY_ADDRESS_IPV6)
    uint64_t routing_prefix_ = 0;
    uint64_t subnet_id_ = 0;
    uint64_t object_id_ = 0;        // separate 64-bit, or 0 when embedded in IPv6
#elif defined(CANOPY_ADDRESS_IPV4)
    uint32_t routing_prefix_ = 0;   // IPv4 address
    uint32_t subnet_id_ = 0;
    uint32_t object_id_ = 0;
#else // CANOPY_ADDRESS_NONE (default)
    uint64_t routing_prefix_ = 0;
    uint64_t subnet_id_ = 0;
    uint64_t object_id_ = 0;
#endif
)__^)

public:
#cpp_quote(R^__(
    // Accessors - same signature regardless of mode
    uint64_t get_routing_prefix() const { return routing_prefix_; }
    void set_routing_prefix(uint64_t val) { routing_prefix_ = val; }
    uint64_t get_subnet() const { return subnet_id_; }
    void set_subnet(uint64_t val) { subnet_id_ = val; }
    uint64_t get_object_id() const { return object_id_; }
    void set_object_id(uint64_t val) { object_id_ = val; }
    // ... constructors, comparison, hash, zone_only(), same_zone(), is_set()
)__^)
};
```

Because the fields are in `#cpp_quote` blocks (invisible to the IDL generator), serialization is handled via custom `serialize()` methods also in `#cpp_quote`. The serialization always writes the logical triplet `{routing_prefix, subnet, object_id}` regardless of internal layout, ensuring wire compatibility across modes.

**Debugger experience**: The private fields are always visible in the debugger with meaningful names (`routing_prefix_`, `subnet_id_`, `object_id_`). The field types may differ between modes (e.g. `uint32_t` in IPv4 mode vs `uint64_t` in IPv6), but the names and intent are consistent.

**Legacy compatibility**: Constructor from `uint64_t` calls `set_subnet(val)`, leaving routing prefix and object at 0. Existing `zone{++zone_gen}` code continues to work.

**Future flexibility**: New address modes can be added by defining a new `#ifdef` block with whatever internal layout is needed (packed arrays, bitfields, etc.), as long as the get/set accessors present the same logical interface.

### get_subnet() Semantics

`get_subnet()` returns the whole address as a `const zone_address&`, not a truncated scalar. This replaces the Phase 1 interim behaviour where `get_subnet()` returned only `subnet_id` as `uint64_t`. Code that needs backward-compatible integer comparison can use `get_address().subnet_id` directly, or rely on `zone_address` comparison operators that handle the "none" mode transparently.

### CMake Configuration (Compile-Time)

All address configuration is set at build time via CMake options. These generate compile-time constants that the `zone_address` methods use for validation, masking, and interpretation. The struct layout itself does not change - only the semantics of how the fields are used.

```cmake
# Address mode: NONE, IPV4, IPV6
set(CANOPY_ADDRESS_MODE "NONE" CACHE STRING "Zone addressing mode")
set_property(CACHE CANOPY_ADDRESS_MODE PROPERTY STRINGS "NONE" "IPV4" "IPV6")

# Field widths (bits) - determines how many bits of each uint64_t field are significant
set(CANOPY_SUBNET_BITS "32" CACHE STRING "Subnet field width in bits (0-64)")
set(CANOPY_OBJECT_BITS "32" CACHE STRING "Object field width in bits (0-64)")

# IPv6-specific: embed object in address or keep separate
option(CANOPY_IPV6_EMBED_OBJECT "Embed object ID within IPv6 address" OFF)
```

These translate to compile-time constants in a generated header:
```cpp
// Generated by CMake
constexpr auto CANOPY_ADDRESS_MODE_VALUE = canopy::address_mode::none;
constexpr uint32_t CANOPY_SUBNET_BITS_VALUE = 32;
constexpr uint32_t CANOPY_OBJECT_BITS_VALUE = 32;
constexpr bool CANOPY_IPV6_EMBED_OBJECT_VALUE = false;
```

The `zone_address` methods (constructors, validators, mask helpers) use these constants. In "none" mode, the `routing_prefix` field is always 0 and the allocator only increments `subnet_id`. In IPv4/IPv6 modes, the routing prefix is set during allocator construction and the field widths determine valid ranges.

### 2. Changes to Zone Types

The four zone types store `zone_address` internally (already done in Phase 1):

**`zone`**: Stores `zone_address` with `object_id = 0`. Used for general zone identity.

**`destination_zone`**: Stores `zone_address` **including** `object_id`. This is the key merge - `destination_zone` carries the full address of a specific object at a specific zone. When used for zone-only routing, `object_id = 0`.

**`caller_zone`**: Stores `zone_address` with `object_id = 0`. Never needs object identity.

**`requesting_zone`**: Stores `zone_address` with `object_id = 0`. Routing hint only.

**Conversion methods** remain: `as_destination()`, `as_caller()`, `as_requesting_zone()`. The `as_destination()` from zone/caller/known copies the address with `object_id = 0`.

**`get_subnet()`** returns `const zone_address&` - the full address. New code and transport layers use this directly.

### 3. Merging destination_zone + object in i_marshaller

Since `destination_zone` now carries `object_id` inside its `zone_address`, the separate `object` parameter is removed from i_marshaller methods:

**Before**:
```cpp
virtual CORO_TASK(int) send(uint64_t protocol_version, encoding encoding, uint64_t tag,
    caller_zone caller_zone_id, destination_zone destination_zone_id, object object_id,
    interface_ordinal interface_id, method method_id, ...);
```

**After**:
```cpp
virtual CORO_TASK(int) send(uint64_t protocol_version, encoding encoding, uint64_t tag,
    caller_zone caller_zone_id, destination_zone destination_zone_id,
    interface_ordinal interface_id, method method_id, ...);
```

The `object_id` is accessed via `destination_zone_id.get_subnet().object_id`.

**`interface_descriptor`** simplifies from `{object, destination_zone}` to just `destination_zone` (which carries both).

**`connection_settings`** loses its separate `object_id` field.

**Methods affected**: `send`, `post`, `try_cast`, `add_ref`, `release`, `object_released`. The `transport_down` method already has no object parameter.

### 4. Wire Protocol IDL Changes

Replace raw `uint64_t` fields with proper types from `rpc_types.idl`. Since tcp.idl and spsc.idl already `#import "rpc/rpc_types.idl"`, the types are available.

**Before** (tcp.idl `call_send`):
```idl
struct call_send {
    rpc::encoding encoding;
    uint64_t tag;
    uint64_t caller_zone_id;
    uint64_t destination_zone_id;
    uint64_t object_id;
    uint64_t interface_id;
    uint64_t method_id;
    ...
};
```

**After**:
```idl
struct call_send {
    rpc::encoding encoding;
    uint64_t tag;
    rpc::caller_zone caller_zone_id;
    rpc::destination_zone destination_zone_id;  // now includes object_id
    rpc::interface_ordinal interface_id;
    rpc::method method_id;
    ...
};
```

This eliminates all the `.get_subnet()` boilerplate in `transports/tcp/src/transport.cpp` (70+ occurrences). The IDL-generated serialization handles the `zone_address` fields automatically.

**Structs to update in each transport IDL**:
- `init_client_channel_send` - `caller_zone_id`, `destination_zone_id`, `adjacent_zone_id` become typed; `caller_object_id` merges into caller info or becomes part of connection_settings
- `init_client_channel_response` - same
- `call_send`, `post_send`, `try_cast_send` - zone/object/interface/method become typed
- `addref_send` - zone types + known_direction become typed, object merges into destination
- `release_send`, `object_released_send`, `transport_down_send` - same pattern

### 5. Multi-Node Address Space

When multiple nodes run on the same physical machine (same `routing_prefix`), the `subnet_id` space must be divided. This is configured via CLI:

```
--subnet-base=0x10000 --subnet-range=0xFFFF
```

The `zone_address_allocator` respects the configured field widths and bounds:
```cpp
class zone_address_allocator
{
    uint64_t routing_prefix_;
    uint64_t subnet_base_;
    uint64_t subnet_range_;
    std::atomic<uint64_t> next_subnet_;

    zone_address allocate_zone();          // returns address with next subnet_id, object_id=0
    zone_address allocate_object(zone_address zone);  // returns address with next object_id
};
```

### 6. Zone Generation Changes

**`service::generate_new_zone_id()`** changes from global atomic counter to using an allocator:

```cpp
// New: allocator-based
zone service::generate_new_zone_id()
{
    return zone{allocator_.allocate_zone()};
}
```

For backward compatibility, the default allocator uses `routing_prefix=0`, `subnet_base=0` and increments `subnet_id` sequentially - identical to current behavior.

### 7. Impact on Routing

**service.cpp maps**: `service_proxies_` and `transports_` keyed by `destination_zone`. Hash and equality now operate on `zone_address`. In the common case (zone-only lookup, `object_id=0`), this works as before. When `destination_zone` carries an `object_id`, the maps are still keyed by zone (the transport doesn't change per-object).

Transport/proxy lookup should compare by zone portion only (routing_prefix + subnet_id), ignoring object_id. Add a `zone_address::same_zone(const zone_address&)` method for this. Or use `as_zone()` to strip the object_id before map lookup.

**transport.cpp `zone_counts_`**: Keyed by `zone` (no object_id), works as-is.

**pass_through.cpp**: `pass_through_key` uses `destination_zone` - should compare by zone portion only.

### 8. Impact on Tests and Demos

**Tests**: In the default configuration (NONE mode, routing_prefix=0), `zone{++zone_gen}` continues to work. The `zone_address` stores `{0, N, 0}` where N is the sequential counter. All 29 zones in complex topology tests work unchanged.

**Demos**: Add CLI options for address mode configuration:
- `--address-mode <none|ipv4|ipv6>` (default: none)
- `--routing-prefix <hex>` (default: 0, meaning local-only)
- `--subnet-base <int>` (default: 0)
- `--subnet-range <int>` (default: max for configured subnet bits)
- Uses `args.hxx` (already used in fuzz_test_main.cpp)

### 9. Migration Phases

| Phase | Scope | Breaking? |
|-------|-------|-----------|
| **1** | Define `zone_address` in IDL. Change zone types to use it internally. Keep `get_subnet()` returning subnet_id. Keep i_marshaller signatures unchanged. | No - `zone{uint64_t}` still works |
| **1b** | Widen `zone_address` fields to `uint64_t`. Update `get_subnet()` to return `const zone_address&`. Add CMake options for address mode and field widths. Fix callsites. | API change for `get_subnet()` |
| **2** | Update wire protocol IDLs to use typed fields. Remove `.get_subnet()` boilerplate in transport code. | Wire format change (version bump) |
| **3** | Merge `object` into `destination_zone` in i_marshaller. Simplify `interface_descriptor`. | API change |
| **4** | Add `zone_address_allocator`, CLI options, multi-node subnet division. | No |
| **5** | IPv6 object embedding logic, prefix-length-based routing (future) | No |

### 10. Files to Modify

**Phase 1** (zone_address introduction) - DONE:
- `rpc/interfaces/rpc/rpc_types.idl` - Added `zone_address` struct, changed zone types' internal storage
- `rpc/include/rpc/internal/types.h` - Updated hash and to_string for zone_address
- `generator/src/protobuf_generator.cpp` - Added nested struct protobuf support

**Phase 1b** (configurable address, get_val change):
- `rpc/interfaces/rpc/rpc_types.idl` - Widen subnet_id and object_id to uint64_t, update get_subnet() to return zone_address
- `rpc/include/rpc/internal/types.h` - Update to_string for wider fields
- `cmake/Canopy.cmake` - Add CANOPY_ADDRESS_MODE, CANOPY_SUBNET_BITS, CANOPY_OBJECT_BITS, CANOPY_IPV6_EMBED_OBJECT
- All callsites using `get_subnet()` as uint64_t - Update to use `get_address()` or `get_subnet().subnet_id`

**Phase 2** (wire protocol):
- `transports/tcp/interface/tcp/tcp.idl` - Replace uint64_t fields with typed structs
- `transports/spsc/interface/spsc/spsc.idl` - Same
- `transports/tcp/src/transport.cpp` - Remove `.get_subnet()` boilerplate (70+ lines simplified)
- `transports/spsc/src/transport.cpp` - Same

**Phase 3** (i_marshaller merge):
- `rpc/include/rpc/internal/marshaller.h` - Remove `object` param from all methods
- `rpc/src/service.cpp` - Update all i_marshaller implementations
- `rpc/src/transport.cpp` - Update all outbound/inbound methods
- `rpc/src/pass_through.cpp` - Update forwarding logic
- `rpc/include/rpc/internal/service.h` - `interface_descriptor` simplification
- `rpc/include/rpc/internal/bindings.h` - Update binding logic
- `rpc/include/rpc/internal/stub.h` - Reference counting adjustments
- All generated proxy/stub code (via IDL generator changes)
- `generator/src/synchronous_generator.cpp` - Generate code using merged address

**Phase 4** (allocator + CLI):
- `rpc/src/service.cpp` - Allocator-based zone generation
- Demo main files - CLI argument additions
- Test fixture files - Optional allocator support

### 11. Risks and Gotchas

- **Transport/proxy map lookup**: Must compare by zone portion only (ignoring object_id) when looking up transports. If `destination_zone` carries object_id, naive map lookup would fail to find the transport.
- **`requesting_zone` routing bug** (canopy-a8i): Orthogonal to this refactor but implementers should be aware of the interaction.
- **IDL generator**: Must handle `zone_address` as a struct with YAS serialization. Since it's a plain struct with three uint64_t fields, the existing generator handles it without changes.
- **Wire format compatibility**: Phase 2 changes the wire format. Need version negotiation so old and new nodes can communicate during rollout. The widened zone_address (24 bytes vs 16 bytes) also changes the wire format in Phase 1b.
- **Performance**: `zone_address` is 24 bytes vs 8 bytes for the original `uint64_t`. Zone types are copied frequently in routing. Profile to verify this is acceptable. In "none" mode, the extra bytes are all zero, so hash computation can short-circuit.
- **get_subnet() migration**: Changing `get_subnet()` from `uint64_t` to `const zone_address&` in Phase 1b requires updating all callsites. The compiler will catch type mismatches, but the volume of changes is significant.

### Verification

- **Phase 1**: Build Debug and Coroutine_Debug presets. `ctest --output-on-failure`. All existing tests must pass - `zone{uint64_t}` constructor ensures backward compat. **DONE**.
- **Phase 1b**: Build Debug and Coroutine_Debug. All tests pass. Verify CMake options are recognised. Verify "none" mode produces identical behavior.
- **Phase 2**: Build with updated wire IDLs. Run TCP and SPSC transport tests.
- **Phase 3**: Build with merged i_marshaller. Full test suite including complex topology tests and fuzz tests.
- **Phase 4**: Run demos with `--address-mode` and `--subnet-base` options. Verify multi-node addressing.
