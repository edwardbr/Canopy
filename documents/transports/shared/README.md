# Shared Transport View

These notes describe transport concepts that should be read as shared Canopy
semantics rather than implementation-status claims.

Best shared-concept entry points today:

- [Hierarchical Transport Pattern](../hierarchical.md)
- [Local Transport](../local.md)
- [Dynamic Library and IPC Child Transports](../dynamic_library.md)
- [SPSC Queues and IPC](../spsc_and_ipc.md)
- [TCP Transport](../tcp.md)
- [Custom Transport](../custom.md)

Interpretation rule:

- transport lifetime semantics, parent/child relationships, passthrough roles,
  and adjacent-zone routing are shared concepts
- exact target names, exact library names, coroutine machinery, SPSC stream
  wiring, and build instructions are usually C++-specific

Current limitation:

- many existing transport documents still mix concept and C++ implementation
  detail in the same file
- read examples and target names as C++ unless stated otherwise
