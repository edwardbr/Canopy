//! Rust counterpart of `c++/rpc/include/rpc/internal/error_codes.h`.
//!
//! The numeric behaviour intentionally mirrors the C++ runtime, including the
//! mutable offset/sign configuration used to adapt the error range.

use std::sync::{Mutex, OnceLock};

#[derive(Debug, Clone, Copy)]
struct ErrorCodeState {
    ok_val: i32,
    offset_val: i32,
    offset_val_is_negative: bool,
}

impl Default for ErrorCodeState {
    fn default() -> Self {
        Self {
            ok_val: 0,
            offset_val: 0,
            offset_val_is_negative: true,
        }
    }
}

fn state() -> &'static Mutex<ErrorCodeState> {
    static STATE: OnceLock<Mutex<ErrorCodeState>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(ErrorCodeState::default()))
}

fn with_state<T>(f: impl FnOnce(&ErrorCodeState) -> T) -> T {
    let guard = state().lock().expect("error code state mutex poisoned");
    f(&guard)
}

fn with_state_mut<T>(f: impl FnOnce(&mut ErrorCodeState) -> T) -> T {
    let mut guard = state().lock().expect("error code state mutex poisoned");
    f(&mut guard)
}

fn offset_code(state: &ErrorCodeState, magnitude: i32) -> i32 {
    state.offset_val
        + if state.offset_val_is_negative {
            -magnitude
        } else {
            magnitude
        }
}

#[allow(non_snake_case)]
pub fn OK() -> i32 {
    with_state(|state| state.ok_val)
}

#[allow(non_snake_case)]
pub fn OUT_OF_MEMORY() -> i32 {
    with_state(|state| offset_code(state, 1))
}

#[allow(non_snake_case)]
pub fn NEED_MORE_MEMORY() -> i32 {
    with_state(|state| offset_code(state, 2))
}

#[allow(non_snake_case)]
pub fn SECURITY_ERROR() -> i32 {
    with_state(|state| offset_code(state, 3))
}

#[allow(non_snake_case)]
pub fn INVALID_DATA() -> i32 {
    with_state(|state| offset_code(state, 4))
}

#[allow(non_snake_case)]
pub fn TRANSPORT_ERROR() -> i32 {
    with_state(|state| offset_code(state, 5))
}

#[allow(non_snake_case)]
pub fn INVALID_METHOD_ID() -> i32 {
    with_state(|state| offset_code(state, 6))
}

#[allow(non_snake_case)]
pub fn INVALID_INTERFACE_ID() -> i32 {
    with_state(|state| offset_code(state, 7))
}

#[allow(non_snake_case)]
pub fn INVALID_CAST() -> i32 {
    with_state(|state| offset_code(state, 8))
}

#[allow(non_snake_case)]
pub fn ZONE_NOT_SUPPORTED() -> i32 {
    with_state(|state| offset_code(state, 9))
}

#[allow(non_snake_case)]
pub fn ZONE_NOT_INITIALISED() -> i32 {
    with_state(|state| offset_code(state, 10))
}

#[allow(non_snake_case)]
pub fn ZONE_NOT_FOUND() -> i32 {
    with_state(|state| offset_code(state, 11))
}

#[allow(non_snake_case)]
pub fn OBJECT_NOT_FOUND() -> i32 {
    with_state(|state| offset_code(state, 12))
}

#[allow(non_snake_case)]
pub fn INVALID_VERSION() -> i32 {
    with_state(|state| offset_code(state, 13))
}

#[allow(non_snake_case)]
pub fn EXCEPTION() -> i32 {
    with_state(|state| offset_code(state, 14))
}

#[allow(non_snake_case)]
pub fn PROXY_DESERIALISATION_ERROR() -> i32 {
    with_state(|state| offset_code(state, 15))
}

#[allow(non_snake_case)]
pub fn STUB_DESERIALISATION_ERROR() -> i32 {
    with_state(|state| offset_code(state, 16))
}

#[allow(non_snake_case)]
pub fn INCOMPATIBLE_SERVICE() -> i32 {
    with_state(|state| offset_code(state, 17))
}

#[allow(non_snake_case)]
pub fn INCOMPATIBLE_SERIALISATION() -> i32 {
    with_state(|state| offset_code(state, 18))
}

#[allow(non_snake_case)]
pub fn REFERENCE_COUNT_ERROR() -> i32 {
    with_state(|state| offset_code(state, 19))
}

#[allow(non_snake_case)]
pub fn SERVICE_PROXY_LOST_CONNECTION() -> i32 {
    with_state(|state| offset_code(state, 21))
}

#[allow(non_snake_case)]
pub fn CALL_CANCELLED() -> i32 {
    with_state(|state| offset_code(state, 22))
}

#[allow(non_snake_case)]
pub fn OBJECT_GONE() -> i32 {
    with_state(|state| offset_code(state, 23))
}

#[allow(non_snake_case)]
pub fn CALL_TIMEOUT() -> i32 {
    with_state(|state| offset_code(state, 24))
}

#[allow(non_snake_case)]
pub fn MIN() -> i32 {
    with_state(|state| state.offset_val + if state.offset_val_is_negative { -24 } else { 1 })
}

#[allow(non_snake_case)]
pub fn MAX() -> i32 {
    with_state(|state| state.offset_val + if state.offset_val_is_negative { -1 } else { 24 })
}

pub fn is_error(err: i32) -> bool {
    err >= MIN() && err <= MAX()
}

pub fn is_critical(err: i32) -> bool {
    is_error(err) && err != OBJECT_GONE() && err != INVALID_CAST()
}

#[allow(non_snake_case)]
pub fn set_OK_val(val: i32) {
    with_state_mut(|state| state.ok_val = val);
}

pub fn set_offset_val(val: i32) {
    with_state_mut(|state| state.offset_val = val);
}

pub fn set_offset_val_is_negative(val: bool) {
    with_state_mut(|state| state.offset_val_is_negative = val);
}

pub fn to_string(err: i32) -> &'static str {
    if err == OK() {
        return "OK";
    }
    if err == OUT_OF_MEMORY() {
        return "out of memory";
    }
    if err == NEED_MORE_MEMORY() {
        return "need more memory";
    }
    if err == SECURITY_ERROR() {
        return "security error";
    }
    if err == INVALID_DATA() {
        return "invalid data";
    }
    if err == TRANSPORT_ERROR() {
        return "transport error";
    }
    if err == INVALID_METHOD_ID() {
        return "invalid method id";
    }
    if err == INVALID_INTERFACE_ID() {
        return "invalid interface id";
    }
    if err == INVALID_CAST() {
        return "invalid cast";
    }
    if err == ZONE_NOT_SUPPORTED() {
        return "zone not supported";
    }
    if err == ZONE_NOT_INITIALISED() {
        return "zone not initialised";
    }
    if err == ZONE_NOT_FOUND() {
        return "zone not found";
    }
    if err == OBJECT_NOT_FOUND() {
        return "object not found";
    }
    if err == INVALID_VERSION() {
        return "invalid version";
    }
    if err == EXCEPTION() {
        return "exception";
    }
    if err == PROXY_DESERIALISATION_ERROR() {
        return "proxy deserialisation error";
    }
    if err == STUB_DESERIALISATION_ERROR() {
        return "stub deserialisation error";
    }
    if err == INCOMPATIBLE_SERVICE() {
        return "service proxy is incompatible with the client";
    }
    if err == INCOMPATIBLE_SERIALISATION() {
        return "service proxy does not support this serialisation format try JSON";
    }
    if err == REFERENCE_COUNT_ERROR() {
        return "reference count error";
    }
    if err == SERVICE_PROXY_LOST_CONNECTION() {
        return "Service proxy has lost connection to the remote service";
    }
    if err == CALL_CANCELLED() {
        return "Service proxy remote call is cancelled";
    }
    if err == OBJECT_GONE() {
        return "The service no longer has an object of that id, perhaps an optimistic pointer call attempt is happining";
    }
    if err == CALL_TIMEOUT() {
        return "outbound RPC call timed out waiting for a response";
    }
    "invalid error code"
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;

    static ERROR_CODE_TEST_LOCK: Mutex<()> = Mutex::new(());

    #[test]
    fn default_values_match_cpp() {
        let _guard = ERROR_CODE_TEST_LOCK
            .lock()
            .expect("error code test mutex poisoned");
        set_OK_val(0);
        set_offset_val(0);
        set_offset_val_is_negative(true);

        assert_eq!(OK(), 0);
        assert_eq!(OUT_OF_MEMORY(), -1);
        assert_eq!(INVALID_METHOD_ID(), -6);
        assert_eq!(CALL_TIMEOUT(), -24);
        assert_eq!(MIN(), -24);
        assert_eq!(MAX(), -1);
    }

    #[test]
    fn positive_offset_mode_matches_cpp() {
        let _guard = ERROR_CODE_TEST_LOCK
            .lock()
            .expect("error code test mutex poisoned");
        set_OK_val(0);
        set_offset_val(100);
        set_offset_val_is_negative(false);

        assert_eq!(OUT_OF_MEMORY(), 101);
        assert_eq!(CALL_TIMEOUT(), 124);
        assert_eq!(MIN(), 101);
        assert_eq!(MAX(), 124);
    }
}
