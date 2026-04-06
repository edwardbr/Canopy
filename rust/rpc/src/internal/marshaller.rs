//! Rust counterpart of `c++/rpc/include/rpc/internal/marshaller.h`.
//!
//! This is the key plumbing interface between zones and the transported data.

use crate::internal::marshaller_params::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
};
use crate::rpc_types::{BackChannelEntry, RemoteObject};

pub fn empty_back_channel() -> &'static [BackChannelEntry] {
    &[]
}

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
