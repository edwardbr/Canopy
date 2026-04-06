//! Runtime protocol version support.
//!
//! This will mirror the role of `c++/rpc/src/version.cpp` and
//! `c++/rpc/include/rpc/internal/version.h`.

pub const VERSION_2: u64 = 2;
pub const VERSION_3: u64 = 3;

pub const LOWEST_SUPPORTED_VERSION: u64 = VERSION_2;
pub const HIGHEST_SUPPORTED_VERSION: u64 = VERSION_3;

pub fn get_version() -> u64 {
    VERSION_3
}
