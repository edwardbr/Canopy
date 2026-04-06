# Rust Transports

This directory will mirror the role of `c++/transports`.

The first transport of interest for cross-language interop is the non-coroutine
dynamic-library transport, but its FFI boundary will need a neutral C ABI.
