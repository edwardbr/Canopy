//! Rust counterpart of `c++/rpc/include/rpc/internal/marshaller.h`.
//!
//! This is the key plumbing interface between zones and the transported data.
//! In Rust, this trait is also the explicit I/O boundary contract:
//! implementations must not hold mutex guards or other runtime locks across any
//! `IMarshaller` request/response-style call. These calls may perform transport
//! I/O, cross an FFI boundary, re-enter local dispatch, or eventually suspend
//! in future async implementations.
//!
//! Coroutine nuance:
//! - blocking mode: no runtime lock may be held across any marshaller call
//! - coroutine mode: the same strict rule applies to request/response paths,
//!   while one-way send-style operations may be relaxed deliberately if they
//!   are proven not to wait for a reply or suspend while holding the lock

use crate::internal::marshaller_params::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
};
use crate::rpc_types::{BackChannelEntry, RemoteObject};

pub fn empty_back_channel() -> &'static [BackChannelEntry] {
    &[]
}

/// Runtime I/O boundary surface.
///
/// Architectural rule, matching the C++ `i_marshaller` intent:
/// callers must snapshot the required state, drop locks, and only then invoke
/// the marshaller boundary. Request/response paths must never be entered with a
/// runtime lock held. One-way send-style calls may only relax that rule in a
/// deliberate coroutine-specific implementation where no wait/reply/suspension
/// can occur while the lock is held.
pub trait IMarshaller {
    fn send(&self, params: SendParams) -> SendResult;

    fn post(&self, params: PostParams);

    fn try_cast(&self, params: TryCastParams) -> StandardResult;

    fn add_ref(&self, params: AddRefParams) -> StandardResult;

    fn release(&self, params: ReleaseParams) -> StandardResult;

    fn object_released(&self, params: ObjectReleasedParams);

    fn transport_down(&self, params: TransportDownParams);

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult;
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RetryBuffer {
    pub data: Vec<u8>,
    pub return_value: i32,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ConnectResult {
    pub error_code: i32,
    pub output_descriptor: RemoteObject,
}

impl ConnectResult {
    pub fn new(error_code: i32, output_descriptor: RemoteObject) -> Self {
        Self {
            error_code,
            output_descriptor,
        }
    }
}
