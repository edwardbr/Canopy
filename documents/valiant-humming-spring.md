# Refactoring Zone Addressing to IPv4/IPv6-Style

## Context

Canopy identifies zones using a simple `uint64_t` counter, which works for single-process testing but doesn't support network-routable addressing or hierarchical subnet-based allocation. The goal is to refactor zone identity to use an IPv4/IPv6-inspired addressing model with structured fields for routing prefix, subnet (zone), and object identity. This enables future TUN-based transport integration, deterministic routing based on address structure, and merging of `destination_zone` + `object_id` into a single address in `i_marshaller`.

The IDL remains the golden definition for all types. Transport wire protocol IDLs (tcp.idl, spsc.idl) will use the typed structs from `rpc_types.idl` instead of raw `uint64_t` fields. The dodgy transport is being removed.

## Current State

**Zone types** in `rpc/interfaces/rpc/rpc_types.idl`: `zone`, `destination_zone`, `caller_zone`, `known_direction_zone` each wrap `uint64_t id`. `object`, `interface_ordinal`, `method` also wrap `uint64_t`.

**i_marshaller** (`rpc/include/rpc/internal/marshaller.h`): Every method takes separate `destination_zone` and `object` parameters. Verified that `destination_zone_id + object_id` always identifies "which object at which zone" across ALL methods (including `object_released` where the message travels in the reverse direction).

**Wire protocols** (`transports/tcp/interface/tcp/tcp.idl`, `transports/spsc/interface/spsc/spsc.idl`): Use raw `uint64_t` fields with manual `.get_val()` extraction in transport implementations. Already `#import "rpc/rpc_types.idl"`.

**Zone generation**: Global `std::atomic<uint64_t>` counter in `service.cpp:32-37`.

**Hash/equality**: `std::hash` specializations in `rpc/include/rpc/internal/types.h` extract `get_val()`.

## Design

### 1. New `zone_address` IDL Type

Define in `rpc/interfaces/rpc/rpc_types.idl` a new struct with debugger-friendly named fields:

```idl
struct zone_address
{
    uint64_t routing_prefix = 0;  // physical node address (IPv6 network prefix)
    uint32_t subnet_id = 0;       // zone ID within the node
    uint32_t object_id = 0;       // object within the zone (0 = zone-only address)

public:
#cpp_quote(R^__(
    // ... constructors, comparison, hash, conversion methods
    // Conversion to/from 128-bit packed form with prefix length
    // Conversion to/from uint64_t for legacy compat (uses subnet_id only)
    // to_string() returning CIDR-like notation
)__^)
};
```

**Total: 128 bits = one IPv6 address.** The layout maps to:
- Bits 0-63: routing prefix (network prefix, identifies the physical node)
- Bits 64-95: subnet ID (zone within the node)
- Bits 96-127: object ID (specific object, or 0 for zone-only)

**Debugger experience**: All three components visible as named struct fields. No bit-unpacking needed.

**Legacy compatibility**: Constructor from `uint64_t` sets `subnet_id = val`, leaving `routing_prefix = 0` and `object_id = 0`. This means existing `zone{++zone_gen}` code continues to work.

**CMake-configurable packed representation**: A CMake option controls whether the in-memory optimized form uses a packed `uint128_t`, `std::array<uint8_t, 16>`, or `std::vector<uint8_t>` for wire serialization and compact storage. The IDL struct with named fields is always the canonical form; the packed representation is a secondary conversion.

### 2. Changes to Zone Types

The four zone types change their internal `uint64_t id` to `zone_address`:

**`zone`**: Stores `zone_address` with `object_id = 0`. Used for general zone identity.

**`destination_zone`**: Stores `zone_address` **including** `object_id`. This is the key merge - `destination_zone` now carries the full address of a specific object at a specific zone. When used for zone-only routing, `object_id = 0`.

**`caller_zone`**: Stores `zone_address` with `object_id = 0`. Never needs object identity.

**`known_direction_zone`**: Stores `zone_address` with `object_id = 0`. Routing hint only.

**Conversion methods** remain: `as_destination()`, `as_caller()`, `as_known_direction_zone()`. The `as_destination()` from zone/caller/known copies the address with `object_id = 0`.

**`get_val()`** remains for backward compatibility, returning `subnet_id` as `uint64_t`. New code should use `get_address()`.

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

The `object_id` is accessed via `destination_zone_id.get_address().object_id`.

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

This eliminates all the `.get_val()` boilerplate in `transports/tcp/src/transport.cpp` (70+ occurrences). The IDL-generated serialization handles the `zone_address` fields automatically.

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

The `zone_address_allocator` respects these bounds:
```cpp
class zone_address_allocator
{
    uint64_t routing_prefix_;
    uint32_t subnet_base_;
    uint32_t subnet_range_;
    std::atomic<uint32_t> next_subnet_;

    zone_address allocate_zone();          // returns address with next subnet_id, object_id=0
    object allocate_object();              // returns next object_id for the current zone
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

**Important**: Transport/proxy lookup should compare by zone portion only (routing_prefix + subnet_id), ignoring object_id. Add a `zone_address::same_zone(const zone_address&)` method for this. Or use `as_zone()` to strip the object_id before map lookup.

**transport.cpp `zone_counts_`**: Keyed by `zone` (no object_id), works as-is.

**pass_through.cpp**: `pass_through_key` uses `destination_zone` - should compare by zone portion only.

### 8. Impact on Tests and Demos

**Tests**: In the default configuration (routing_prefix=0), `zone{++zone_gen}` continues to work. The `zone_address` stores `{0, N, 0}` where N is the sequential counter. All 29 zones in complex topology tests work unchanged.

**Demos**: Add CLI options for IPv6-mode configuration:
- `--routing-prefix <hex>` (default: 0, meaning local-only)
- `--subnet-base <int>` (default: 0)
- `--subnet-range <int>` (default: 0xFFFFFFFF)
- Uses `args.hxx` (already used in fuzz_test_main.cpp)

### 9. Migration Phases

| Phase | Scope | Breaking? |
|-------|-------|-----------|
| **1** | Define `zone_address` in IDL. Change zone types to use it internally. Keep `get_val()`. Keep i_marshaller signatures unchanged. | No - `zone{uint64_t}` still works |
| **2** | Update wire protocol IDLs to use typed fields. Remove `.get_val()` boilerplate in transport code. | Wire format change (version bump) |
| **3** | Merge `object` into `destination_zone` in i_marshaller. Simplify `interface_descriptor`. | API change |
| **4** | Add `zone_address_allocator`, CLI options, multi-node subnet division. | No |
| **5** | CMake-configurable packed representations, prefix-length-based routing (future) | No |

### 10. Files to Modify

**Phase 1** (zone_address introduction):
- `rpc/interfaces/rpc/rpc_types.idl` - Add `zone_address` struct, change zone types' internal storage
- `rpc/include/rpc/internal/types.h` - Update hash and to_string for zone_address
- `cmake/Canopy.cmake` - New CMake option for packed representation

**Phase 2** (wire protocol):
- `transports/tcp/interface/tcp/tcp.idl` - Replace uint64_t fields with typed structs
- `transports/spsc/interface/spsc/spsc.idl` - Same
- `transports/tcp/src/transport.cpp` - Remove `.get_val()` boilerplate (70+ lines simplified)
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
- **`known_direction_zone` routing bug** (canopy-a8i): Orthogonal to this refactor but implementers should be aware of the interaction.
- **IDL generator**: Must handle `zone_address` as a struct with YAS serialization. Since it's a plain struct with uint64_t + uint32_t + uint32_t, the existing generator should handle it without changes.
- **Wire format compatibility**: Phase 2 changes the wire format. Need version negotiation so old and new nodes can communicate during rollout.
- **Performance**: `zone_address` is 16 bytes vs 8 bytes for `uint64_t`. Zone types are copied frequently in routing. Hashing 16 bytes is ~2x cost. Profile to verify this is acceptable. Consider caching hash values.

### Verification

- **Phase 1**: Build Debug and Coroutine_Debug presets. `ctest --output-on-failure`. All existing tests must pass - `zone{uint64_t}` constructor ensures backward compat.
- **Phase 2**: Build with updated wire IDLs. Run TCP and SPSC transport tests.
- **Phase 3**: Build with merged i_marshaller. Full test suite including complex topology tests and fuzz tests.
- **Phase 4**: Run demos with `--routing-prefix` and `--subnet-base` options. Verify multi-node addressing.
