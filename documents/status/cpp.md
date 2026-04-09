# C++ Implementation Status

The C++ implementation is the primary and most complete Canopy implementation.

Current characteristics:

- supports both blocking and coroutine builds
- supports the full main transport tree currently present in `c++/transports/`
- supports YAS and Protocol Buffers
- remains the implementation that repository-wide architecture and transport
  documentation should treat as authoritative unless explicitly stated
  otherwise

When documentation describes Canopy behavior without qualification, it should
normally be read as describing the C++ implementation.
