# C++ Architecture View

These notes describe how to read the architecture documentation from the
perspective of the primary C++ implementation.

The C++ implementation is the authoritative implementation for:

- blocking and coroutine runtime behavior
- full current transport support
- YAS and Protocol Buffers
- SGX support
- stream-backed transport behavior

Use these detailed documents as C++-primary references:

- [Architecture Overview](../01-overview.md)
- [Transports and Passthroughs](../06-transports-and-passthroughs.md)
- [Passthroughs](../passthroughs.md)

When examples, class names, coroutine macros, build targets, or transport
library names appear without qualification, interpret them as C++ references.
