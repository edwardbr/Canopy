//! Rust-side dynamic-library transport bridge for the shared `c_abi` surface.

pub mod adapter;
pub mod context;
pub mod entrypoints;
pub mod ffi;
pub mod loader;
mod platform_ffi;

pub use adapter::ChildTransportAdapter;
pub use adapter::ParentTransportAdapter;
pub use context::DllContext;
pub use context::InitChildZoneResult;
pub use context::init_child_zone;
pub use entrypoints::dll_add_ref;
pub use entrypoints::dll_destroy;
pub use entrypoints::dll_free_new_zone_id_result;
pub use entrypoints::dll_free_send_result;
pub use entrypoints::dll_free_standard_result;
pub use entrypoints::dll_get_new_zone_id;
pub use entrypoints::dll_init;
pub use entrypoints::dll_object_released;
pub use entrypoints::dll_post;
pub use entrypoints::dll_release;
pub use entrypoints::dll_send;
pub use entrypoints::dll_transport_down;
pub use entrypoints::dll_try_cast;
pub use ffi::*;
pub use loader::DynamicLibrary;
pub use loader::DynamicLibraryExports;
pub use loader::LoadedChild;
