//! Raw platform FFI for dynamic-library loading.
//!
//! Keep OS loader unsafety here so the parent-side loader API can stay safe.

use std::ffi::{CStr, CString, c_char, c_void};

#[cfg(unix)]
unsafe extern "C"
{
    fn dlopen(filename: *const c_char, flags: i32) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlclose(handle: *mut c_void) -> i32;
    fn dlerror() -> *const c_char;
}

#[cfg(unix)]
const RTLD_NOW: i32 = 2;
#[cfg(unix)]
const RTLD_LOCAL: i32 = 0;

#[cfg(unix)]
pub(crate) unsafe fn open_local_now(path: &CString) -> *mut c_void
{
    unsafe { dlopen(path.as_ptr(), RTLD_NOW | RTLD_LOCAL) }
}

#[cfg(unix)]
pub(crate) unsafe fn resolve_symbol(handle: *mut c_void, symbol: &CStr) -> *mut c_void
{
    unsafe { dlsym(handle, symbol.as_ptr()) }
}

#[cfg(unix)]
pub(crate) unsafe fn close(handle: *mut c_void)
{
    let _ = unsafe { dlclose(handle) };
}

#[cfg(unix)]
pub(crate) unsafe fn last_error() -> String
{
    let raw = unsafe { dlerror() };
    if raw.is_null() {
        "unknown dlopen error".to_string()
    } else {
        unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned()
    }
}

#[cfg(unix)]
pub(crate) unsafe fn load_symbol<T: Copy>(
    handle: *mut c_void,
    symbol: &CStr,
) -> Result<T, String>
{
    let raw = unsafe { resolve_symbol(handle, symbol) };
    if raw.is_null() {
        Err(unsafe { last_error() })
    } else {
        Ok(unsafe { std::mem::transmute_copy(&raw) })
    }
}

#[cfg(not(unix))]
compile_error!("canopy-transport-dynamic-library currently supports Unix-like platforms only");
