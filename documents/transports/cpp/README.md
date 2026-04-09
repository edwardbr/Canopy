# C++ Transport View

These notes describe how to read the transport documentation from the
perspective of the primary C++ implementation.

Use these documents as C++-primary transport references:

- [Dynamic Library and IPC Child Transports](../dynamic_library.md)
- [SPSC Queues and IPC](../spsc_and_ipc.md)
- [TCP Transport](../tcp.md)
- [SGX](../sgx.md)
- [Stream Backpressure Guidelines](../stream_backpressure_guidelines.md)

The C++ implementation is currently the authoritative implementation for:

- coroutine-capable stream-backed transports
- TCP
- SPSC and IPC transport behavior
- SGX transport behavior
- the detailed transport build/runtime target names described in the documents
