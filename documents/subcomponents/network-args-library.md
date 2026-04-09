# Network Arguments Library (`canopy_network_config`)

## Context

All demo binaries and test hosts that create a `zone_address_allocator` need to parse address-family flags and routing-prefix arguments. Rather than duplicating this parsing logic across each binary, a shared static library (`canopy_network_config`) provides a common API. This keeps `args.hxx` boilerplate and the IPv4→IPv6 conversion in one place.

## CMake Configuration

Read this section against the current `interfaces/rpc/rpc_types.idl` and the
live `zone_address` implementation.
`zone_address` is now a versioned blob format whose capability header records
the address layout, including `subnet_size_bits` and `object_id_size_bits`.

The packed representation for wire serialization is configured at build time:

```cmake
option(CANOPY_ADDRESS_PACKED "Use packed 128-bit representation" OFF)
```

Do not treat the old fixed structured form as authoritative. The authoritative
address model is:

- routing prefix
- subnet
- local/object address

with the exact field widths described by the versioned address capabilities in
`interfaces/rpc/rpc_types.idl`.

## CLI Address-Family Options

### Address-family flag

Mutually exclusive; controls how `--routing-prefix` is parsed:

| Flag | Meaning |
|------|---------|
| `-4` | `--routing-prefix` is an IPv4 address in dotted-decimal notation (`a.b.c.d`) |
| `-6` | `--routing-prefix` is an IPv6 address in colon-hex notation (`2001:db8::1`) |
| *(neither)* | Local-only mode: `routing_prefix=0`, inter-node routing disabled |

### Routing and subnet options

| Option | Default | Description |
|--------|---------|-------------|
| `--routing-prefix <addr>` | `0` | This node's network address. Format determined by `-4`/`-6` (see conversion below). If omitted, `detect_routing_prefix()` selects the best interface address automatically. |
| `--subnet-base <int>` | `0` | First `subnet_id` allocated by this process. |
| `--subnet-range <int>` | `0xFFFFFFFF` | Number of `subnet_id` values reserved for this process. |

### Telemetry options

Consistent with `test_host/main.cpp`:

| Option | Default | Description |
|--------|---------|-------------|
| `--telemetry-console` | off | Enable console telemetry output |
| `--console-path <path>` | `telemetry/reports/` | Telemetry report output directory |

## Routing Prefix Conversion

### IPv4 → `routing_prefix` (6to4-inspired, RFC 3056)

```
routing_prefix = 0x2002 << 48 | (uint64_t)ipv4_addr << 16
```

For example `192.168.1.1` (`0xC0A80101`):

```
routing_prefix = 0x2002C0A801010000
```

The `0x2002` well-known prefix occupies bits 63–48, the 32-bit IPv4 address occupies bits 47–16, and bits 15–0 are zero (available for future site-subnet use). This encoding is unambiguous, reversible, and avoids collision with the all-zeros local prefix.

### IPv6 → `routing_prefix`

The first 64 bits of the address are taken directly as the routing prefix (equivalent to a /64 network prefix).

## Public API

**Location:** `c++/subcomponents/network_config/`

**CMake target:** `canopy_network_config` (static library)

**Headers:**
- `c++/subcomponents/network_config/include/canopy/network_config/network_args.h`

```cpp
namespace canopy::network_config {

struct network_config
{
    std::vector<named_virtual_address> virtual_addresses;
    std::vector<tcp_endpoint> listen_endpoints;
    std::vector<tcp_endpoint> connect_endpoints;
};

// Add the address-family group and routing/subnet args to an existing ArgumentParser.
// Call this before parser.ParseCLI(argc, argv).
network_args_context add_network_args(args::ArgumentParser& parser);

// Extract a network_config from the context returned by add_network_args().
// Throws std::invalid_argument on bad address format.
network_config get_network_config(const network_args_context& ctx);

// Convenience: parse and return in one step.
network_config parse_network_args(
    int argc,
    char* argv[],
    args::ArgumentParser& parser);

// Build a zone_address_allocator from a network_config.
zone_address_allocator make_allocator(const network_config& cfg);

// Low-level converters (also useful in tests).
uint64_t ipv4_to_routing_prefix(const std::string& dotted_decimal);  // 6to4 mapping
uint64_t ipv6_to_routing_prefix(const std::string& colon_hex);       // first 64 bits

// Auto-detect routing-prefix candidates from the host's network interfaces.
bool detect_routing_prefix(...);

}
```

`add_network_args` registers the following arguments:

```
-4, -6                  mutually exclusive group (address family)
--routing-prefix <addr> address in dotted-decimal (IPv4) or colon-hex (IPv6) format;
                        if omitted, detect_routing_prefix() selects the best interface address
--subnet-base <int>
--subnet-range <int>
--telemetry-console
--console-path <path>
```

## Routing Prefix Auto-Detection

When `--routing-prefix` is not provided, `detect_routing_prefix()` enumerates the host's network interfaces using POSIX `getifaddrs()` (Linux/macOS) and selects an address using the following priority:

| Priority | Criterion |
|----------|-----------|
| 1 | Globally-routable unicast IPv6 (non-link-local `fe80::/10`, non-loopback `::1`) |
| 2 | Public IPv4 (not loopback `127.x`, link-local `169.254.x`, or RFC 1918 private ranges) |
| 3 | Private IPv4 (RFC 1918: `10.x`, `172.16-31.x`, `192.168.x`) |
| 4 | Returns `0` — local-only mode, no routing prefix |

The address-family flag (`-4`/`-6`) constrains which address families are considered during auto-detection when `--routing-prefix` is absent. If neither flag is given, IPv6 is tried first, then IPv4.

The selected address is converted to a `routing_prefix` using the same rules as explicit `--routing-prefix` (6to4 mapping for IPv4, first 64 bits for IPv6).

## Usage Pattern

```cpp
#include <canopy/network_config/network_args.h>

int main(int argc, char* argv[])
{
    args::ArgumentParser parser("TCP Transport Demo");
    auto net_ctx = canopy::network_config::add_network_args(parser);
    // add any demo-specific args here ...

    try { parser.ParseCLI(argc, argv); }
    catch (args::Help&)          { std::cout << parser; return 0; }
    catch (args::ParseError& e)  { std::cerr << e.what() << "\n" << parser; return 1; }

    // If --routing-prefix was not supplied, get_network_config() calls
    // detect_routing_prefix() automatically to select the best interface address.
    auto cfg       = canopy::network_config::get_network_config(net_ctx);
    auto allocator = canopy::network_config::make_allocator(cfg);

    auto service = std::make_shared<rpc::root_service>("server", allocator.allocate_zone(), scheduler);
    // ...
}
```

## Files

```
c++/subcomponents/network_config/
  CMakeLists.txt
  include/canopy/network_config/network_args.h
  src/network_args.cpp           ← CLI parsing, conversion, get_network_config()
  src/network_auto_detect.cpp    ← detect_routing_prefix() via getifaddrs()
```

**Dependencies:** `args` (INTERFACE target from `submodules/args`), `rpc_types_idl` (for `zone_address`, `zone`, `object`).

## Consumers

Every binary that creates a `zone_address_allocator` links `canopy_network_config`:

| Binary | Location | Notes |
|--------|----------|-------|
| `local_transport_demo` | `demos/comprehensive/src/transport/` | Replace `zone_gen` atomic counter |
| `spsc_transport_demo` | `demos/comprehensive/src/transport/` | Replace fixed `zone{1}`/`zone{2}` |
| `tcp_transport_demo` | `demos/comprehensive/src/transport/` | Replace split-range zone IDs; also expose `--port`/`--host` |
| `benchmark` | `demos/comprehensive/src/transport/` | Same as tcp_transport_demo |
| `websocket_server` | `demos/websocket/server/` | Migrate from manual arg parsing to `args.hxx` first |
| `rpc_test` | `tests/test_host/main.cpp` | Already uses `args.hxx`; add `add_network_args()` call |

## Demo Zone Strategy Updates

The comprehensive transport demos (`demos/comprehensive/src/transport/`) currently hardcode all zone IDs and network parameters. Each must be updated to accept the CLI options above and delegate to `canopy_network_config`:

| Demo | Current zone strategy | Change required |
|------|-----------------------|-----------------|
| `local_transport_demo` | `std::atomic<uint64_t> zone_gen{0}`, `++zone_gen` | Replace with `allocator.allocate_zone()` |
| `spsc_transport_demo` | Fixed: `zone{1}`, `zone{2}` | Replace with allocator; add `main(int argc, char* argv[])` |
| `tcp_transport_demo` | Server `zone{1}`, client `zone{100+}`; port/host hardcoded | Replace zones with allocator; expose `--port`/`--host` |
| `benchmark` | Same split-range pattern as TCP demo | Replace zones with allocator; expose `--port` |
