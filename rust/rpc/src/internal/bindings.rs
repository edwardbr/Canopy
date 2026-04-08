//! Rust counterpart of `c++/rpc/include/rpc/internal/bindings.h`.
//!
//! Pointer ownership types live in `remote_pointer.rs`. This module keeps the
//! binding helper logic layered on top of those pointer types.

pub use crate::internal::remote_pointer::{
    BindableInterfaceValue, BoundInterface, Optimistic, OptimisticPtr, Shared, SharedPtr,
    add_ref_options_for_pointer_kind, bind_gone, bind_incoming_optimistic, bind_incoming_shared,
    bind_local_optimistic_from_shared, bind_local_optimistic_from_weak, bind_local_value,
    bind_null, bind_outgoing_interface, bind_remote_value, is_bound_pointer_gone,
    is_bound_pointer_null, null_remote_descriptor, release_options_for_pointer_kind,
};
