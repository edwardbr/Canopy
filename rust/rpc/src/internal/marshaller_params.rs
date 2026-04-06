//! Rust counterpart of `c++/rpc/include/rpc/internal/marshaller_params.h`.

use crate::rpc_types::{
    AddRefOptions, BackChannelEntry, CallerZone, DestinationZone, Encoding, InterfaceOrdinal,
    Method, ReleaseOptions, RemoteObject, RequestingZone, Zone,
};

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SendParams {
    pub protocol_version: u64,
    pub encoding_type: Encoding,
    pub tag: u64,
    pub caller_zone_id: CallerZone,
    pub remote_object_id: RemoteObject,
    pub interface_id: InterfaceOrdinal,
    pub method_id: Method,
    pub in_data: Vec<u8>,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct PostParams {
    pub protocol_version: u64,
    pub encoding_type: Encoding,
    pub tag: u64,
    pub caller_zone_id: CallerZone,
    pub remote_object_id: RemoteObject,
    pub interface_id: InterfaceOrdinal,
    pub method_id: Method,
    pub in_data: Vec<u8>,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TryCastParams {
    pub protocol_version: u64,
    pub caller_zone_id: CallerZone,
    pub remote_object_id: RemoteObject,
    pub interface_id: InterfaceOrdinal,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AddRefParams {
    pub protocol_version: u64,
    pub remote_object_id: RemoteObject,
    pub caller_zone_id: CallerZone,
    pub requesting_zone_id: RequestingZone,
    pub build_out_param_channel: AddRefOptions,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ReleaseParams {
    pub protocol_version: u64,
    pub remote_object_id: RemoteObject,
    pub caller_zone_id: CallerZone,
    pub options: ReleaseOptions,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ObjectReleasedParams {
    pub protocol_version: u64,
    pub remote_object_id: RemoteObject,
    pub caller_zone_id: CallerZone,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TransportDownParams {
    pub protocol_version: u64,
    pub destination_zone_id: DestinationZone,
    pub caller_zone_id: CallerZone,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct GetNewZoneIdParams {
    pub protocol_version: u64,
    pub in_back_channel: Vec<BackChannelEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct StandardResult {
    pub error_code: i32,
    pub out_back_channel: Vec<BackChannelEntry>,
}

impl StandardResult {
    pub fn new(error_code: i32, out_back_channel: Vec<BackChannelEntry>) -> Self {
        Self {
            error_code,
            out_back_channel,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SendResult {
    pub error_code: i32,
    pub out_buf: Vec<u8>,
    pub out_back_channel: Vec<BackChannelEntry>,
}

impl SendResult {
    pub fn new(error_code: i32, out_buf: Vec<u8>, out_back_channel: Vec<BackChannelEntry>) -> Self {
        Self {
            error_code,
            out_buf,
            out_back_channel,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct NewZoneIdResult {
    pub error_code: i32,
    pub zone_id: Zone,
    pub out_back_channel: Vec<BackChannelEntry>,
}

impl NewZoneIdResult {
    pub fn new(error_code: i32, zone_id: Zone, out_back_channel: Vec<BackChannelEntry>) -> Self {
        Self {
            error_code,
            zone_id,
            out_back_channel,
        }
    }
}
