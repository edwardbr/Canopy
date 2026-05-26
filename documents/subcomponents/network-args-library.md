# Network Arguments Library (`canopy_network_config`)

## Context

Demo binaries that accept physical network endpoints or named virtual addresses
use `canopy_network_config` rather than duplicating `args.hxx` parsing,
address-family handling, and routing-prefix auto-detection. The library builds
`rpc::zone_address` values and `rpc::zone_id_allocator` instances for callers
that need more than the local `rpc::DEFAULT_PREFIX`.

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

## CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--va-name <identifier>` | required | Name for a virtual zone address. Repeat once per virtual address. |
| `--va-type <local\|ipv4\|ipv6\|ipv6_tun>` | required | Address type for the matching virtual address. |
| `--va-prefix <addr>` | auto-detect | Routing prefix for the matching virtual address. IPv4 and IPv6 formats are inferred from `--va-type`. |
| `--va-subnet-bits <n>` | `64` | Width of the subnet field for the matching virtual address. |
| `--va-subnet <value>` | `0` | Initial subnet value for the matching virtual address. |
| `--va-object-id-bits <n>` | `64` | Width of the object-id field for the matching virtual address. |
| `--va-object-id <value>` | `0` | Initial object id for the matching virtual address. |
| `--listen [name:]addr:port` | none | Physical endpoint to bind. The optional name maps the endpoint to a virtual address. |
| `--connect [name:]addr:port` | none | Physical endpoint to connect to. The optional name maps the endpoint to a virtual address. |

The `--va-*` options are repeatable and matched by occurrence index. `--va-name`
and `--va-type` must have the same count. Missing trailing values for the other
virtual-address options use their defaults. Endpoint address family is inferred
from the endpoint string: `192.168.1.1:8080` for IPv4,
`[2001:db8::1]:8080` for IPv6, and a bare port for `0.0.0.0:<port>`.

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

// Add virtual-address and endpoint args to an existing ArgumentParser.
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

// Build a zone_id_allocator from the first virtual address in a network_config.
rpc::zone_id_allocator make_allocator(const network_config& cfg);

// Build a zone_address from a named virtual address.
rpc::zone_address get_zone_address(
    const network_config& cfg,
    const std::string& name = "");

// Low-level endpoint and address helpers.
tcp_endpoint parse_named_endpoint(const std::string& name_host_port);
void ipv4_to_ip_address(const std::string& dotted_decimal, ip_address& addr);
void ipv6_to_ip_address(const std::string& colon_hex, ip_address& addr);
uint64_t ip_address_to_uint64(const ip_address& addr, ip_address_family family);

// Auto-detect routing-prefix candidates from the host's network interfaces.
bool detect_routing_prefix(...);

}
```

`add_network_args` registers the following arguments:

```
--va-name <identifier>
--va-type <local|ipv4|ipv6|ipv6_tun>
--va-prefix <addr>
--va-subnet-bits <n>
--va-subnet <value>
--va-object-id-bits <n>
--va-object-id <value>
--listen [name:]addr:port
--connect [name:]addr:port
--telemetry-console
--console-path <path>
```

## Routing Prefix Auto-Detection

When a virtual address omits `--va-prefix`, `detect_routing_prefix()`
enumerates the host's network interfaces using POSIX `getifaddrs()`
(Linux/macOS) and selects an address using the following priority:

| Priority | Criterion |
|----------|-----------|
| 1 | Globally-routable unicast IPv6 (non-link-local `fe80::/10`, non-loopback `::1`) |
| 2 | Public IPv4 (not loopback `127.x`, link-local `169.254.x`, or RFC 1918 private ranges) |
| 3 | Private IPv4 (RFC 1918: `10.x`, `172.16-31.x`, `192.168.x`) |
| 4 | Returns `0` — local-only mode, no routing prefix |

The selected address is converted to a `routing_prefix` using the same rules as
an explicit virtual-address prefix: 6to4 mapping for IPv4, first 64 bits for
IPv6.

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

    // If --va-prefix was not supplied, get_network_config() calls
    // detect_routing_prefix() automatically to select the best interface address.
    auto cfg       = canopy::network_config::get_network_config(net_ctx);
    auto allocator = canopy::network_config::make_allocator(cfg);

    rpc::zone_address zone_addr;
    auto error = allocator.allocate_zone(zone_addr);
    if (error != rpc::error::OK())
        return 1;

    auto service = rpc::root_service::create("server", rpc::zone{zone_addr}, scheduler);
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

**Dependencies:** `args` (INTERFACE target from `submodules/args`), `rpc`
(for `zone_address`, `zone`, `zone_id_allocator`, and related generated
types).

## Consumers

Current binaries that link `canopy_network_config`:

| Binary | Location | Notes |
|--------|----------|-------|
| `tcp_transport_demo` | `c++/demos/comprehensive/src/transport/tcp/` | Uses `--listen`, optional `--connect`, and allocator-derived server/client zones. |
| `websocket_server` | `c++/demos/websocket/server/` | Uses network args for virtual address and listen endpoint setup. |
| `tcp_spsc_tls_demo` | `c++/demos/stream_composition/src/` | Uses network args for listen/connect endpoints and allocator-derived zones. |

## Demo Zone Strategy Updates

The comprehensive transport demos no longer use fixed integer-zone
constructors. Current strategy:

| Demo | Current zone strategy |
|------|-----------------------|
| `local_transport_demo` | Uses `rpc::DEFAULT_PREFIX` for an in-process local root zone. |
| `spsc_transport_demo` | Uses `rpc::zone_id_allocator{rpc::DEFAULT_PREFIX}` and allocates two local zones. |
| `tcp_transport_demo` | Uses `canopy_network_config`, `--listen`, optional `--connect`, and allocator-derived server/client zones. |
