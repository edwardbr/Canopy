//! Serialization-format-specific support for the Rust Canopy RPC runtime.
//!
//! Keep format-specific metadata and helpers here so they do not contaminate
//! the generic transport, binding, and lifetime layers.

pub mod protobuf;
