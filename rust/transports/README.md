# Rust Transports

This directory mirrors the role of `c++/transports`.

Current Rust transport crates:

- `dynamic_library`: C ABI dynamic-library transport support. The child-side
  endpoint forwards decoded C ABI calls into a Rust `Service`, while helper
  code registers the parent callback transport for hierarchical DLL setups.
- `local`: in-process local parent/child transport pair. This mirrors the C++
  local transport pattern for creating child zones without making parent/child
  relationships part of the generic `canopy_rpc::Transport` abstraction.

The generic Rust `Transport` in `canopy-rpc` remains neutral so
non-hierarchical transports can follow the same service/proxy/stub routing
model without parent concepts.
