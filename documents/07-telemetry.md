<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Telemetry

Scope note:

- this document describes the current C++ telemetry subsystem
- the telemetry interfaces are C++ implementation details, not a cross-language
  Canopy guarantee
- see [C++ Status](status/cpp.md), [Rust Status](status/rust.md), and
  [JavaScript Status](status/javascript.md) for implementation scope

Canopy includes a comprehensive telemetry subsystem for debugging,
visualization, and performance analysis in the primary C++ implementation.

## 1. Overview

The telemetry system captures all RPC operations and can output to multiple formats:

| Service | Format | Purpose |
|---------|--------|---------|
| Console | ANSI text | Real-time debugging |
| Sequence Diagram | PlantUML (.pu) | Sequence visualization |
| Animation | HTML5/D3.js | Interactive playback |
| Multiplexing | Multiple outputs | Combined logging |

## 2. Enabling Telemetry

### Compile-Time Enable

```bash
cmake --preset Debug
```

`CANOPY_USE_TELEMETRY` is already enabled by the current `Debug` preset. Some
reduced-logging and release-oriented presets disable it.

Telemetry service headers live under:

```text
c++/telemetry/include/rpc/telemetry/
```

### Programmatic Setup

```cpp
#ifdef CANOPY_USE_TELEMETRY
std::shared_ptr<rpc::i_telemetry_service> telemetry_service;

rpc::console_telemetry_service::create(
    telemetry_service,
    "my_test_suite",    // Test suite name
    "test_calculator",   // Test name
    "/tmp/rpc_logs"      // Output directory
);

rpc::set_telemetry_service(telemetry_service);
#endif
```

## 3. Console Telemetry

### Features

- ANSI color-coded output
- Real-time topology diagram
- spdlog integration
- Optional file output

### Setup

```cpp
std::shared_ptr<rpc::i_telemetry_service> console_service;
rpc::console_telemetry_service::create(
    console_service,
    suite_name,
    test_name,
    "/tmp/rpc_logs");
```

The concrete interface surface is defined in:

- [i_telemetry_service.h](/var/home/edward/projects/Canopy/c++/telemetry/include/rpc/telemetry/i_telemetry_service.h)

### Sample Output

```
[RED] [HOST_SERVICE = 1] service_creation
[YELLOW] [ZONE 1] === TOPOLOGY DIAGRAM ===
  Zone 1: HOST_SERVICE
  Zone 2: (child of 1) REMOTE_SERVICE
=========================
[GREEN] [object_proxy zone 1 destination 2 object 1] add_ref
[BLUE] [service_proxy zone 1 -> 2] send
```

## 4. Sequence Diagram Telemetry

### Features

- PlantUML format for sequence diagrams
- Color-coded participants
- Thread ID tracking
- Health checks at end

### Setup

```cpp
std::shared_ptr<rpc::i_telemetry_service> sequence_service;
rpc::sequence_diagram_telemetry_service::create(
    sequence_service,
    suite_name,
    test_name,
    "/tmp/rpc_logs");
```

### Generated Output (PlantUML)

```plantuml
@startuml
title TestSuite.test_calculator

participant "[HOST_SERVICE zone 1]" as s1 order 100 #Moccasin
activate s1 #Moccasin

participant "[object_proxy zone 1 destination 2 object 1]" as op_1_2_1 order 101 #pink
s1 -> op_1_2_1 : add_ref

note left #green: DEBUG: Object reference added

deactivate s1
system is healthy
@enduml
```

### Viewing Diagrams

1. Save as `.pu` file
2. Open with PlantUML tool
3. Or use online renderer at plantuml.com

## 5. Animation Telemetry

### Features

- Interactive HTML5 visualization
- D3.js-powered animation
- Playback controls (start/stop/reset)
- Speed slider
- Timeline scrubber
- Event log panel with filtering

### Setup

```cpp
std::shared_ptr<rpc::i_telemetry_service> animation_service;
rpc::animation_telemetry_service::create(
    animation_service,
    suite_name,
    test_name,
    "/tmp/rpc_logs");
```

### Generated Output

Creates a self-contained HTML file with:
- Zone hierarchy visualization
- Node types (zones, services, proxies, stubs)
- Link types (contains, route, channel)
- Color-coded by node type
- Zoom and pan support
- Type visibility filters

## 6. Multiplexing Service

### Purpose

Forward telemetry events to multiple services simultaneously.

### Setup

```cpp
std::shared_ptr<rpc::i_telemetry_service> console_service;
std::shared_ptr<rpc::i_telemetry_service> sequence_service;
std::shared_ptr<rpc::i_telemetry_service> animation_service;

// Create individual services
rpc::console_telemetry_service::create(console_service, ...);
rpc::sequence_diagram_telemetry_service::create(sequence_service, ...);
rpc::animation_telemetry_service::create(animation_service, ...);

// Create multiplexer
std::shared_ptr<rpc::i_telemetry_service> multiplexer;
rpc::multiplexing_telemetry_service::create(
    multiplexer,
    {console_service, sequence_service, animation_service});
```

## 7. Captured Events

### Service Events

```cpp
on_service_creation(name, zone_id, parent_zone_id)
on_service_deletion(name, zone_id)
on_service_send(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            interface_id,
            method_id)
on_service_post(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            interface_id,
            method_id)
on_service_try_cast(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            interface_id)
on_service_add_ref(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            requesting_zone_id,
            options)
on_service_release(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id,
            options)
on_service_object_released(zone_id,
            remote_object remote_object_id,  // includes zone and object_id
            caller_zone_id)
on_service_transport_down(zone_id,
            destination_zone destination_zone_id,  // zone-only, no object_id
            caller_zone_id)
```

### Proxy And Transport Events

```cpp
on_service_proxy_creation(service_name, service_proxy_name, local_zone, remote_zone, caller_zone)
on_service_proxy_deletion(local_zone, remote_zone, caller_zone)
on_service_proxy_send(local_zone, remote_object, caller_zone, interface_id, method_id)
on_object_proxy_creation(local_zone, remote_zone, object_id)
on_interface_proxy_creation(local_zone, remote_zone, object_id, interface_id)
on_transport_creation(name, local_zone, adjacent_zone, status)
on_transport_deletion(local_zone, adjacent_zone)
on_transport_status_change(name, local_zone, adjacent_zone, old_status, new_status)
on_transport_outbound_send(local_zone, adjacent_zone, remote_object, caller_zone, interface_id, method_id)
on_transport_inbound_send(local_zone, adjacent_zone, remote_object, caller_zone, interface_id, method_id)
```

## 8. Thread-Local Logging

For crash diagnostics, enable thread-local logging:

```bash
cmake --preset Debug -DCANOPY_USE_THREAD_LOCAL_LOGGING=ON
```

### Features

- Circular buffer per thread
- Automatic dump on assertion failure
- Files in `/tmp/rpc_debug_dumps/`

### Sample Crash Report

```
=== Canopy Thread-Local Crash Report ===
Timestamp: 2024-01-15 10:30:45

Thread: 140735123456789
Buffer size: 1024 entries

Entry 0: [DEBUG] service_creation zone=1
Entry 1: [DEBUG] object_proxy_creation zone=1->2 object=1
Entry 2: [ERROR] send failed error=5
...
```

## 9. Integration with Tests

Telemetry is built when `CANOPY_USE_TELEMETRY=ON`. In the current C++ build
that produces:

- `rpc_telemetry_interface`
- `rpc_telemetry`

and, in enclave builds:

- `rpc_telemetry_enclave`

### Test Fixture Integration

```cpp
class my_test : public testing::Test
{
protected:
    void SetUp() override
    {
#ifdef CANOPY_USE_TELEMETRY
        rpc::console_telemetry_service::create(
            telemetry_service_,
            "my_test_suite",
            "my_test",
            "/tmp/rpc_logs");
        rpc::set_telemetry_service(telemetry_service_);
#endif
    }

    void TearDown() override
    {
#ifdef CANOPY_USE_TELEMETRY
        rpc::set_telemetry_service(nullptr);
        telemetry_service_.reset();
#endif
    }

#ifdef CANOPY_USE_TELEMETRY
    std::shared_ptr<rpc::i_telemetry_service> telemetry_service_;
#endif
};
```

## 10. Performance Considerations

### When to Use

- **Development/Debugging**: Always enable for complex issues
- **Testing**: Enable for test debugging
- **Production**: Disable (significant overhead)

### Overhead

| Service | Overhead |
|---------|----------|
| Console | Medium (I/O bound) |
| Sequence Diagram | Low (memory) |
| Animation | Medium (DOM updates) |
| Multiplexing | Multiplies base service overhead |

## 11. Best Practices

1. **Disable in production** - Telemetry has significant overhead
2. **Use multiplexing sparingly** - Multiple outputs = more overhead
3. **Enable selectively** - Only capture what you need
4. **Use animation for complex flows** - Visual debugging is powerful
5. **Combine with logging** - Console + file for comprehensive coverage

## 12. Next Steps

- [Memory Management](architecture/04-memory-management.md) - Understanding lifecycle
- [Zone Hierarchies](architecture/07-zone-hierarchies.md) - Complex topologies
- [Examples](10-examples.md) - Working code with telemetry
