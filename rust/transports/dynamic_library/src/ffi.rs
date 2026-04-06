//! Raw FFI surface for the shared `c_abi/dynamic_library` ABI.
//!
//! This module intentionally contains the `repr(C)` types, raw pointer decoding
//! helpers, and foreign callback invocation layer. Keep unsafety here.

use crate::context::DllContext;
use canopy_rpc::{
    AddRefParams, BackChannelEntry, Encoding, GetNewZoneIdParams, IMarshaller, NewZoneIdResult,
    ObjectReleasedParams, PostParams, ReleaseParams, RemoteObject, SendParams, SendResult, StandardResult,
    TransportDownParams, TryCastParams, Zone, ZoneAddress,
};
use canopy_rpc::internal::error_codes;
use canopy_rpc::rpc_types::ConnectionSettings;

use std::ffi::{c_char, c_void};
use std::marker::PhantomData;
use std::mem::size_of;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyByteBuffer
{
    pub data: *mut u8,
    pub size: usize,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyConstByteBuffer
{
    pub data: *const u8,
    pub size: usize,
}

impl CanopyConstByteBuffer
{
    pub fn from_slice(data: &[u8]) -> Self
    {
        Self {
            data: data.as_ptr(),
            size: data.len(),
        }
    }

    pub(crate) fn as_slice<'a>(&self) -> &'a [u8]
    {
        if self.data.is_null() || self.size == 0 {
            &[]
        } else {
            // SAFETY: the FFI caller guarantees the buffer is readable for the call duration.
            unsafe { std::slice::from_raw_parts(self.data, self.size) }
        }
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyBackChannelEntry
{
    pub type_id: u64,
    pub payload: CanopyConstByteBuffer,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyBackChannelSpan
{
    pub data: *const CanopyBackChannelEntry,
    pub size: usize,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyMutBackChannelSpan
{
    pub data: *mut CanopyBackChannelEntry,
    pub size: usize,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyZoneAddress
{
    pub blob: CanopyConstByteBuffer,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyZone
{
    pub address: CanopyZoneAddress,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyRemoteObject
{
    pub address: CanopyZoneAddress,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyConnectionSettings
{
    pub inbound_interface_id: u64,
    pub outbound_interface_id: u64,
    pub remote_object_id: CanopyRemoteObject,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyStandardResult
{
    pub error_code: i32,
    pub out_back_channel: CanopyMutBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopySendResult
{
    pub error_code: i32,
    pub out_buf: CanopyByteBuffer,
    pub out_back_channel: CanopyMutBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyNewZoneIdResult
{
    pub error_code: i32,
    pub zone_id: CanopyZone,
    pub out_back_channel: CanopyMutBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopySendParams
{
    pub protocol_version: u64,
    pub encoding_type: u64,
    pub tag: u64,
    pub caller_zone_id: CanopyZone,
    pub remote_object_id: CanopyRemoteObject,
    pub interface_id: u64,
    pub method_id: u64,
    pub in_data: CanopyConstByteBuffer,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyPostParams
{
    pub protocol_version: u64,
    pub encoding_type: u64,
    pub tag: u64,
    pub caller_zone_id: CanopyZone,
    pub remote_object_id: CanopyRemoteObject,
    pub interface_id: u64,
    pub method_id: u64,
    pub in_data: CanopyConstByteBuffer,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyTryCastParams
{
    pub protocol_version: u64,
    pub caller_zone_id: CanopyZone,
    pub remote_object_id: CanopyRemoteObject,
    pub interface_id: u64,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyAddRefParams
{
    pub protocol_version: u64,
    pub remote_object_id: CanopyRemoteObject,
    pub caller_zone_id: CanopyZone,
    pub requesting_zone_id: CanopyZone,
    pub build_out_param_channel: u8,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyReleaseParams
{
    pub protocol_version: u64,
    pub remote_object_id: CanopyRemoteObject,
    pub caller_zone_id: CanopyZone,
    pub options: u8,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyObjectReleasedParams
{
    pub protocol_version: u64,
    pub remote_object_id: CanopyRemoteObject,
    pub caller_zone_id: CanopyZone,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyTransportDownParams
{
    pub protocol_version: u64,
    pub destination_zone_id: CanopyZone,
    pub caller_zone_id: CanopyZone,
    pub in_back_channel: CanopyBackChannelSpan,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CanopyGetNewZoneIdParams
{
    pub protocol_version: u64,
    pub in_back_channel: CanopyBackChannelSpan,
}

pub type CanopyParentContext = *mut c_void;
pub type CanopyChildContext = *mut c_void;

pub type CanopyAllocFn = unsafe extern "C" fn(allocator_ctx: *mut c_void, size: usize) -> CanopyByteBuffer;
pub type CanopyFreeFn = unsafe extern "C" fn(allocator_ctx: *mut c_void, data: *mut u8, size: usize);

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct CanopyAllocatorVtable
{
    pub allocator_ctx: *mut c_void,
    pub alloc: Option<CanopyAllocFn>,
    pub free: Option<CanopyFreeFn>,
}

pub type CanopyParentSendFn =
    unsafe extern "C" fn(parent_ctx: CanopyParentContext, params: *const CanopySendParams, result: *mut CanopySendResult) -> i32;
pub type CanopyParentPostFn =
    unsafe extern "C" fn(parent_ctx: CanopyParentContext, params: *const CanopyPostParams);
pub type CanopyParentTryCastFn = unsafe extern "C" fn(
    parent_ctx: CanopyParentContext,
    params: *const CanopyTryCastParams,
    result: *mut CanopyStandardResult,
) -> i32;
pub type CanopyParentAddRefFn = unsafe extern "C" fn(
    parent_ctx: CanopyParentContext,
    params: *const CanopyAddRefParams,
    result: *mut CanopyStandardResult,
) -> i32;
pub type CanopyParentReleaseFn = unsafe extern "C" fn(
    parent_ctx: CanopyParentContext,
    params: *const CanopyReleaseParams,
    result: *mut CanopyStandardResult,
) -> i32;
pub type CanopyParentObjectReleasedFn =
    unsafe extern "C" fn(parent_ctx: CanopyParentContext, params: *const CanopyObjectReleasedParams);
pub type CanopyParentTransportDownFn =
    unsafe extern "C" fn(parent_ctx: CanopyParentContext, params: *const CanopyTransportDownParams);
pub type CanopyParentGetNewZoneIdFn = unsafe extern "C" fn(
    parent_ctx: CanopyParentContext,
    params: *const CanopyGetNewZoneIdParams,
    result: *mut CanopyNewZoneIdResult,
) -> i32;

pub type CanopyDllInitFn = unsafe extern "C" fn(params: *mut CanopyDllInitParams) -> i32;
pub type CanopyDllDestroyFn = unsafe extern "C" fn(child_ctx: CanopyChildContext);
pub type CanopyDllSendFn =
    unsafe extern "C" fn(child_ctx: CanopyChildContext, params: *const CanopySendParams, result: *mut CanopySendResult) -> i32;
pub type CanopyDllPostFn =
    unsafe extern "C" fn(child_ctx: CanopyChildContext, params: *const CanopyPostParams);
pub type CanopyDllTryCastFn = unsafe extern "C" fn(
    child_ctx: CanopyChildContext,
    params: *const CanopyTryCastParams,
    result: *mut CanopyStandardResult,
) -> i32;
pub type CanopyDllAddRefFn = unsafe extern "C" fn(
    child_ctx: CanopyChildContext,
    params: *const CanopyAddRefParams,
    result: *mut CanopyStandardResult,
) -> i32;
pub type CanopyDllReleaseFn = unsafe extern "C" fn(
    child_ctx: CanopyChildContext,
    params: *const CanopyReleaseParams,
    result: *mut CanopyStandardResult,
) -> i32;
pub type CanopyDllObjectReleasedFn =
    unsafe extern "C" fn(child_ctx: CanopyChildContext, params: *const CanopyObjectReleasedParams);
pub type CanopyDllTransportDownFn =
    unsafe extern "C" fn(child_ctx: CanopyChildContext, params: *const CanopyTransportDownParams);
pub type CanopyDllGetNewZoneIdFn = unsafe extern "C" fn(
    child_ctx: CanopyChildContext,
    params: *const CanopyGetNewZoneIdParams,
    result: *mut CanopyNewZoneIdResult,
) -> i32;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct CanopyDllInitParams
{
    pub name: *const c_char,
    pub parent_zone: CanopyZone,
    pub child_zone: CanopyZone,
    pub input_descr: *const CanopyConnectionSettings,
    pub parent_ctx: CanopyParentContext,
    pub allocator: CanopyAllocatorVtable,
    pub parent_send: Option<CanopyParentSendFn>,
    pub parent_post: Option<CanopyParentPostFn>,
    pub parent_try_cast: Option<CanopyParentTryCastFn>,
    pub parent_add_ref: Option<CanopyParentAddRefFn>,
    pub parent_release: Option<CanopyParentReleaseFn>,
    pub parent_object_released: Option<CanopyParentObjectReleasedFn>,
    pub parent_transport_down: Option<CanopyParentTransportDownFn>,
    pub parent_get_new_zone_id: Option<CanopyParentGetNewZoneIdFn>,
    pub child_ctx: CanopyChildContext,
    pub output_obj: CanopyRemoteObject,
}

pub struct BorrowedConnectionSettings<'a>
{
    raw: CanopyConnectionSettings,
    _remote_object_blob: PhantomData<&'a RemoteObject>,
}

impl<'a> BorrowedConnectionSettings<'a>
{
    pub fn new(settings: &'a ConnectionSettings) -> Self
    {
        Self {
            raw: CanopyConnectionSettings {
                inbound_interface_id: settings.inbound_interface_id.get_val(),
                outbound_interface_id: settings.outbound_interface_id.get_val(),
                remote_object_id: borrow_remote_object(&settings.remote_object_id),
            },
            _remote_object_blob: PhantomData,
        }
    }

    pub fn as_raw(&self) -> &CanopyConnectionSettings
    {
        &self.raw
    }
}

pub struct BorrowedBackChannel<'a>
{
    entries: Vec<CanopyBackChannelEntry>,
    _marker: PhantomData<&'a [BackChannelEntry]>,
}

impl<'a> BorrowedBackChannel<'a>
{
    pub fn new(entries: &'a [BackChannelEntry]) -> Self
    {
        let entries = entries
            .iter()
            .map(|entry| CanopyBackChannelEntry {
                type_id: entry.type_id,
                payload: CanopyConstByteBuffer::from_slice(&entry.payload),
            })
            .collect();

        Self {
            entries,
            _marker: PhantomData,
        }
    }

    pub fn as_raw(&self) -> CanopyBackChannelSpan
    {
        CanopyBackChannelSpan {
            data: self.entries.as_ptr(),
            size: self.entries.len(),
        }
    }
}

macro_rules! define_borrowed_params {
    ($name:ident, $raw:ty, $params:ty, $builder:expr) => {
        pub struct $name<'a>
        {
            back_channel: BorrowedBackChannel<'a>,
            raw: $raw,
        }

        impl<'a> $name<'a>
        {
            pub fn new(params: &'a $params) -> Self
            {
                let back_channel = BorrowedBackChannel::new(&params.in_back_channel);
                let raw = $builder(params, &back_channel);
                Self { back_channel, raw }
            }

            pub fn as_raw(&self) -> &$raw
            {
                let _ = &self.back_channel;
                &self.raw
            }
        }
    };
}

define_borrowed_params!(BorrowedSendParams, CanopySendParams, SendParams, |params: &SendParams,
                                                                        back_channel: &BorrowedBackChannel| CanopySendParams {
    protocol_version: params.protocol_version,
    encoding_type: params.encoding_type as u64,
    tag: params.tag,
    caller_zone_id: borrow_zone(&params.caller_zone_id),
    remote_object_id: borrow_remote_object(&params.remote_object_id),
    interface_id: params.interface_id.get_val(),
    method_id: params.method_id.get_val(),
    in_data: CanopyConstByteBuffer::from_slice(&params.in_data),
    in_back_channel: back_channel.as_raw(),
});

define_borrowed_params!(BorrowedPostParams, CanopyPostParams, PostParams, |params: &PostParams,
                                                                        back_channel: &BorrowedBackChannel| CanopyPostParams {
    protocol_version: params.protocol_version,
    encoding_type: params.encoding_type as u64,
    tag: params.tag,
    caller_zone_id: borrow_zone(&params.caller_zone_id),
    remote_object_id: borrow_remote_object(&params.remote_object_id),
    interface_id: params.interface_id.get_val(),
    method_id: params.method_id.get_val(),
    in_data: CanopyConstByteBuffer::from_slice(&params.in_data),
    in_back_channel: back_channel.as_raw(),
});

define_borrowed_params!(BorrowedTryCastParams, CanopyTryCastParams, TryCastParams, |params: &TryCastParams,
                                                                                      back_channel: &BorrowedBackChannel| CanopyTryCastParams {
    protocol_version: params.protocol_version,
    caller_zone_id: borrow_zone(&params.caller_zone_id),
    remote_object_id: borrow_remote_object(&params.remote_object_id),
    interface_id: params.interface_id.get_val(),
    in_back_channel: back_channel.as_raw(),
});

define_borrowed_params!(BorrowedAddRefParams, CanopyAddRefParams, AddRefParams, |params: &AddRefParams,
                                                                                  back_channel: &BorrowedBackChannel| CanopyAddRefParams {
    protocol_version: params.protocol_version,
    remote_object_id: borrow_remote_object(&params.remote_object_id),
    caller_zone_id: borrow_zone(&params.caller_zone_id),
    requesting_zone_id: borrow_zone(&params.requesting_zone_id),
    build_out_param_channel: params.build_out_param_channel.0,
    in_back_channel: back_channel.as_raw(),
});

define_borrowed_params!(BorrowedReleaseParams, CanopyReleaseParams, ReleaseParams, |params: &ReleaseParams,
                                                                                      back_channel: &BorrowedBackChannel| CanopyReleaseParams {
    protocol_version: params.protocol_version,
    remote_object_id: borrow_remote_object(&params.remote_object_id),
    caller_zone_id: borrow_zone(&params.caller_zone_id),
    options: params.options.0,
    in_back_channel: back_channel.as_raw(),
});

define_borrowed_params!(
    BorrowedObjectReleasedParams,
    CanopyObjectReleasedParams,
    ObjectReleasedParams,
    |params: &ObjectReleasedParams, back_channel: &BorrowedBackChannel| CanopyObjectReleasedParams {
        protocol_version: params.protocol_version,
        remote_object_id: borrow_remote_object(&params.remote_object_id),
        caller_zone_id: borrow_zone(&params.caller_zone_id),
        in_back_channel: back_channel.as_raw(),
    }
);

define_borrowed_params!(
    BorrowedTransportDownParams,
    CanopyTransportDownParams,
    TransportDownParams,
    |params: &TransportDownParams, back_channel: &BorrowedBackChannel| CanopyTransportDownParams {
        protocol_version: params.protocol_version,
        destination_zone_id: borrow_zone(&params.destination_zone_id),
        caller_zone_id: borrow_zone(&params.caller_zone_id),
        in_back_channel: back_channel.as_raw(),
    }
);

define_borrowed_params!(
    BorrowedGetNewZoneIdParams,
    CanopyGetNewZoneIdParams,
    GetNewZoneIdParams,
    |params: &GetNewZoneIdParams, back_channel: &BorrowedBackChannel| CanopyGetNewZoneIdParams {
        protocol_version: params.protocol_version,
        in_back_channel: back_channel.as_raw(),
    }
);

fn borrow_zone_address(value: &ZoneAddress) -> CanopyZoneAddress
{
    CanopyZoneAddress {
        blob: CanopyConstByteBuffer::from_slice(value.get_blob()),
    }
}

pub fn borrow_zone(value: &Zone) -> CanopyZone
{
    CanopyZone {
        address: borrow_zone_address(value.get_address()),
    }
}

pub fn borrow_remote_object(value: &RemoteObject) -> CanopyRemoteObject
{
    CanopyRemoteObject {
        address: borrow_zone_address(value.get_address()),
    }
}

fn copy_back_channel(data: *const CanopyBackChannelEntry, size: usize) -> Vec<BackChannelEntry>
{
    if data.is_null() || size == 0 {
        return Vec::new();
    }

    let slice = unsafe { std::slice::from_raw_parts(data, size) };
    slice
        .iter()
        .map(|entry| BackChannelEntry {
            type_id: entry.type_id,
            payload: if entry.payload.data.is_null() || entry.payload.size == 0 {
                Vec::new()
            } else {
                unsafe { std::slice::from_raw_parts(entry.payload.data, entry.payload.size).to_vec() }
            },
        })
        .collect()
}

fn copy_zone_address(value: CanopyZoneAddress) -> ZoneAddress
{
    if value.blob.data.is_null() || value.blob.size == 0 {
        ZoneAddress::default()
    } else {
        ZoneAddress::new(unsafe { std::slice::from_raw_parts(value.blob.data, value.blob.size).to_vec() })
    }
}

fn copy_zone(value: CanopyZone) -> Zone
{
    Zone::new(copy_zone_address(value.address))
}

pub fn copy_remote_object(value: CanopyRemoteObject) -> RemoteObject
{
    RemoteObject::new(copy_zone_address(value.address))
}

fn copy_encoding(value: u64) -> Result<Encoding, i32>
{
    match value {
        1 => Ok(Encoding::YasBinary),
        2 => Ok(Encoding::YasCompressedBinary),
        8 => Ok(Encoding::YasJson),
        16 => Ok(Encoding::ProtocolBuffers),
        _ => Err(error_codes::INVALID_DATA()),
    }
}

pub fn copy_connection_settings(raw: &CanopyConnectionSettings) -> ConnectionSettings
{
    ConnectionSettings {
        inbound_interface_id: canopy_rpc::InterfaceOrdinal::new(raw.inbound_interface_id),
        outbound_interface_id: canopy_rpc::InterfaceOrdinal::new(raw.outbound_interface_id),
        remote_object_id: copy_remote_object(raw.remote_object_id),
    }
}

pub fn copy_init_connection_settings(params: &CanopyDllInitParams) -> Option<ConnectionSettings>
{
    if params.input_descr.is_null() {
        None
    } else {
        let raw = unsafe { &*params.input_descr };
        Some(copy_connection_settings(raw))
    }
}

pub fn copy_send_params(raw: &CanopySendParams) -> Result<SendParams, i32>
{
    Ok(SendParams {
        protocol_version: raw.protocol_version,
        encoding_type: copy_encoding(raw.encoding_type)?,
        tag: raw.tag,
        caller_zone_id: copy_zone(raw.caller_zone_id),
        remote_object_id: copy_remote_object(raw.remote_object_id),
        interface_id: canopy_rpc::InterfaceOrdinal::new(raw.interface_id),
        method_id: canopy_rpc::Method::new(raw.method_id),
        in_data: raw.in_data.as_slice().to_vec(),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    })
}

pub fn copy_post_params(raw: &CanopyPostParams) -> Result<PostParams, i32>
{
    Ok(PostParams {
        protocol_version: raw.protocol_version,
        encoding_type: copy_encoding(raw.encoding_type)?,
        tag: raw.tag,
        caller_zone_id: copy_zone(raw.caller_zone_id),
        remote_object_id: copy_remote_object(raw.remote_object_id),
        interface_id: canopy_rpc::InterfaceOrdinal::new(raw.interface_id),
        method_id: canopy_rpc::Method::new(raw.method_id),
        in_data: raw.in_data.as_slice().to_vec(),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    })
}

pub fn copy_try_cast_params(raw: &CanopyTryCastParams) -> TryCastParams
{
    TryCastParams {
        protocol_version: raw.protocol_version,
        caller_zone_id: copy_zone(raw.caller_zone_id),
        remote_object_id: copy_remote_object(raw.remote_object_id),
        interface_id: canopy_rpc::InterfaceOrdinal::new(raw.interface_id),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    }
}

pub fn copy_add_ref_params(raw: &CanopyAddRefParams) -> AddRefParams
{
    AddRefParams {
        protocol_version: raw.protocol_version,
        remote_object_id: copy_remote_object(raw.remote_object_id),
        caller_zone_id: copy_zone(raw.caller_zone_id),
        requesting_zone_id: copy_zone(raw.requesting_zone_id),
        build_out_param_channel: canopy_rpc::AddRefOptions(raw.build_out_param_channel),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    }
}

pub fn copy_release_params(raw: &CanopyReleaseParams) -> ReleaseParams
{
    ReleaseParams {
        protocol_version: raw.protocol_version,
        remote_object_id: copy_remote_object(raw.remote_object_id),
        caller_zone_id: copy_zone(raw.caller_zone_id),
        options: canopy_rpc::ReleaseOptions(raw.options),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    }
}

pub fn copy_object_released_params(raw: &CanopyObjectReleasedParams) -> ObjectReleasedParams
{
    ObjectReleasedParams {
        protocol_version: raw.protocol_version,
        remote_object_id: copy_remote_object(raw.remote_object_id),
        caller_zone_id: copy_zone(raw.caller_zone_id),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    }
}

pub fn copy_transport_down_params(raw: &CanopyTransportDownParams) -> TransportDownParams
{
    TransportDownParams {
        protocol_version: raw.protocol_version,
        destination_zone_id: copy_zone(raw.destination_zone_id),
        caller_zone_id: copy_zone(raw.caller_zone_id),
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    }
}

pub fn copy_get_new_zone_id_params(raw: &CanopyGetNewZoneIdParams) -> GetNewZoneIdParams
{
    GetNewZoneIdParams {
        protocol_version: raw.protocol_version,
        in_back_channel: copy_back_channel(raw.in_back_channel.data, raw.in_back_channel.size),
    }
}

pub fn copy_standard_result(raw: &CanopyStandardResult) -> StandardResult
{
    StandardResult {
        error_code: raw.error_code,
        out_back_channel: copy_back_channel(raw.out_back_channel.data, raw.out_back_channel.size),
    }
}

pub fn copy_send_result(raw: &CanopySendResult) -> SendResult
{
    SendResult {
        error_code: raw.error_code,
        out_buf: if raw.out_buf.data.is_null() || raw.out_buf.size == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(raw.out_buf.data, raw.out_buf.size).to_vec() }
        },
        out_back_channel: copy_back_channel(raw.out_back_channel.data, raw.out_back_channel.size),
    }
}

pub fn copy_new_zone_id_result(raw: &CanopyNewZoneIdResult) -> NewZoneIdResult
{
    NewZoneIdResult {
        error_code: raw.error_code,
        zone_id: Zone::new(copy_zone_address(raw.zone_id.address)),
        out_back_channel: copy_back_channel(raw.out_back_channel.data, raw.out_back_channel.size),
    }
}

fn alloc_bytes(allocator: &CanopyAllocatorVtable, len: usize) -> Result<CanopyByteBuffer, i32>
{
    if len == 0 {
        return Ok(CanopyByteBuffer::default());
    }

    let Some(alloc) = allocator.alloc else {
        return Err(error_codes::OUT_OF_MEMORY());
    };

    let buffer = unsafe { alloc(allocator.allocator_ctx, len) };
    if buffer.data.is_null() || buffer.size < len {
        if !buffer.data.is_null() {
            if let Some(free) = allocator.free {
                unsafe { free(allocator.allocator_ctx, buffer.data, buffer.size) };
            }
        }
        return Err(error_codes::OUT_OF_MEMORY());
    }

    Ok(buffer)
}

fn free_bytes(allocator: &CanopyAllocatorVtable, buffer: &mut CanopyByteBuffer)
{
    if buffer.data.is_null() || buffer.size == 0 {
        *buffer = CanopyByteBuffer::default();
        return;
    }

    if let Some(free) = allocator.free {
        unsafe { free(allocator.allocator_ctx, buffer.data, buffer.size) };
    }

    *buffer = CanopyByteBuffer::default();
}

fn alloc_const_buffer_from_slice(
    allocator: &CanopyAllocatorVtable,
    bytes: &[u8],
) -> Result<CanopyConstByteBuffer, i32>
{
    let raw = alloc_bytes(allocator, bytes.len())?;
    if !bytes.is_empty() {
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), raw.data, bytes.len()) };
    }
    Ok(CanopyConstByteBuffer {
        data: raw.data.cast_const(),
        size: bytes.len(),
    })
}

fn free_const_buffer(allocator: &CanopyAllocatorVtable, buffer: &mut CanopyConstByteBuffer)
{
    let mut owned = CanopyByteBuffer {
        data: buffer.data.cast_mut(),
        size: buffer.size,
    };
    free_bytes(allocator, &mut owned);
    *buffer = CanopyConstByteBuffer::default();
}

fn alloc_back_channel(
    allocator: &CanopyAllocatorVtable,
    entries: &[BackChannelEntry],
) -> Result<CanopyMutBackChannelSpan, i32>
{
    if entries.is_empty() {
        return Ok(CanopyMutBackChannelSpan::default());
    }

    let bytes_len = size_of::<CanopyBackChannelEntry>() * entries.len();
    let raw_entries = alloc_bytes(allocator, bytes_len)?;
    let entries_ptr = raw_entries.data.cast::<CanopyBackChannelEntry>();

    for (index, entry) in entries.iter().enumerate() {
        let payload = match alloc_const_buffer_from_slice(allocator, &entry.payload) {
            Ok(payload) => payload,
            Err(error_code) => {
                for rollback_index in 0..index {
                    let rollback_entry = unsafe { &mut *entries_ptr.add(rollback_index) };
                    free_const_buffer(allocator, &mut rollback_entry.payload);
                }
                let mut raw_entries = raw_entries;
                free_bytes(allocator, &mut raw_entries);
                return Err(error_code);
            }
        };

        unsafe {
            entries_ptr.add(index).write(CanopyBackChannelEntry {
                type_id: entry.type_id,
                payload,
            });
        }
    }

    Ok(CanopyMutBackChannelSpan {
        data: entries_ptr,
        size: entries.len(),
    })
}

fn free_back_channel(allocator: &CanopyAllocatorVtable, span: &mut CanopyMutBackChannelSpan)
{
    if span.data.is_null() || span.size == 0 {
        *span = CanopyMutBackChannelSpan::default();
        return;
    }

    for index in 0..span.size {
        let entry = unsafe { &mut *span.data.add(index) };
        free_const_buffer(allocator, &mut entry.payload);
    }

    let mut raw_entries = CanopyByteBuffer {
        data: span.data.cast(),
        size: size_of::<CanopyBackChannelEntry>() * span.size,
    };
    free_bytes(allocator, &mut raw_entries);
    *span = CanopyMutBackChannelSpan::default();
}

pub fn write_remote_object(
    allocator: &CanopyAllocatorVtable,
    value: &RemoteObject,
) -> Result<CanopyRemoteObject, i32>
{
    Ok(CanopyRemoteObject {
        address: CanopyZoneAddress {
            blob: alloc_const_buffer_from_slice(allocator, value.get_address().get_blob())?,
        },
    })
}

pub fn write_zone(
    allocator: &CanopyAllocatorVtable,
    value: &Zone,
) -> Result<CanopyZone, i32>
{
    Ok(CanopyZone {
        address: CanopyZoneAddress {
            blob: alloc_const_buffer_from_slice(allocator, value.get_address().get_blob())?,
        },
    })
}

pub fn free_remote_object(allocator: &CanopyAllocatorVtable, value: &mut CanopyRemoteObject)
{
    free_const_buffer(allocator, &mut value.address.blob);
    *value = CanopyRemoteObject::default();
}

pub fn free_zone(allocator: &CanopyAllocatorVtable, value: &mut CanopyZone)
{
    free_const_buffer(allocator, &mut value.address.blob);
    *value = CanopyZone::default();
}

pub fn write_standard_result(
    allocator: &CanopyAllocatorVtable,
    value: &StandardResult,
    out: &mut CanopyStandardResult,
) -> Result<(), i32>
{
    out.error_code = value.error_code;
    out.out_back_channel = alloc_back_channel(allocator, &value.out_back_channel)?;
    Ok(())
}

pub fn write_send_result(
    allocator: &CanopyAllocatorVtable,
    value: &SendResult,
    out: &mut CanopySendResult,
) -> Result<(), i32>
{
    out.error_code = value.error_code;
    out.out_buf = alloc_bytes(allocator, value.out_buf.len())?;
    if !value.out_buf.is_empty() {
        unsafe { std::ptr::copy_nonoverlapping(value.out_buf.as_ptr(), out.out_buf.data, value.out_buf.len()) };
    }

    match alloc_back_channel(allocator, &value.out_back_channel) {
        Ok(back_channel) => {
            out.out_back_channel = back_channel;
            Ok(())
        }
        Err(error_code) => {
            free_bytes(allocator, &mut out.out_buf);
            Err(error_code)
        }
    }
}

pub fn write_new_zone_id_result(
    allocator: &CanopyAllocatorVtable,
    value: &NewZoneIdResult,
    out: &mut CanopyNewZoneIdResult,
) -> Result<(), i32>
{
    out.error_code = value.error_code;
    out.zone_id = write_zone(allocator, &value.zone_id)?;
    match alloc_back_channel(allocator, &value.out_back_channel) {
        Ok(back_channel) => {
            out.out_back_channel = back_channel;
            Ok(())
        }
        Err(error_code) => {
            free_zone(allocator, &mut out.zone_id);
            Err(error_code)
        }
    }
}

pub fn free_standard_result(allocator: &CanopyAllocatorVtable, value: &mut CanopyStandardResult)
{
    free_back_channel(allocator, &mut value.out_back_channel);
    value.error_code = 0;
}

pub fn free_send_result(allocator: &CanopyAllocatorVtable, value: &mut CanopySendResult)
{
    free_bytes(allocator, &mut value.out_buf);
    free_back_channel(allocator, &mut value.out_back_channel);
    value.error_code = 0;
}

pub fn free_new_zone_id_result(allocator: &CanopyAllocatorVtable, value: &mut CanopyNewZoneIdResult)
{
    free_zone(allocator, &mut value.zone_id);
    free_back_channel(allocator, &mut value.out_back_channel);
    value.error_code = 0;
}

pub fn write_init_output_obj(
    params: &mut CanopyDllInitParams,
    output_obj: &RemoteObject,
) -> Result<(), i32>
{
    params.output_obj = write_remote_object(&params.allocator, output_obj)?;
    Ok(())
}

pub fn box_child_context<M>(context: DllContext<M>) -> CanopyChildContext
where
    M: IMarshaller,
{
    Box::into_raw(Box::new(context)).cast()
}

pub fn with_child_context<M, R>(
    child_ctx: CanopyChildContext,
    f: impl FnOnce(&DllContext<M>) -> R,
) -> Option<R>
where
    M: IMarshaller,
{
    if child_ctx.is_null() {
        return None;
    }

    let context = unsafe { &*(child_ctx.cast::<DllContext<M>>()) };
    if context.is_destroyed() {
        None
    } else {
        Some(f(context))
    }
}

pub fn destroy_child_context<M>(child_ctx: CanopyChildContext) -> bool
where
    M: IMarshaller,
{
    if child_ctx.is_null() {
        return false;
    }

    let context = unsafe { Box::from_raw(child_ctx.cast::<DllContext<M>>()) };
    if !context.destroy() {
        std::mem::forget(context);
        return false;
    }

    true
}

#[derive(Debug, Clone, Copy, Default)]
pub struct ParentCallbacks
{
    pub parent_ctx: CanopyParentContext,
    pub send: Option<CanopyParentSendFn>,
    pub post: Option<CanopyParentPostFn>,
    pub try_cast: Option<CanopyParentTryCastFn>,
    pub add_ref: Option<CanopyParentAddRefFn>,
    pub release: Option<CanopyParentReleaseFn>,
    pub object_released: Option<CanopyParentObjectReleasedFn>,
    pub transport_down: Option<CanopyParentTransportDownFn>,
    pub get_new_zone_id: Option<CanopyParentGetNewZoneIdFn>,
}

impl ParentCallbacks
{
    pub fn from_init_params(params: &CanopyDllInitParams) -> Self
    {
        Self {
            parent_ctx: params.parent_ctx,
            send: params.parent_send,
            post: params.parent_post,
            try_cast: params.parent_try_cast,
            add_ref: params.parent_add_ref,
            release: params.parent_release,
            object_released: params.parent_object_released,
            transport_down: params.parent_transport_down,
            get_new_zone_id: params.parent_get_new_zone_id,
        }
    }
}

impl IMarshaller for ParentCallbacks
{
    fn send(&self, params: SendParams) -> SendResult
    {
        let Some(callback) = self.send else {
            return SendResult::new(error_codes::ZONE_NOT_FOUND(), Vec::new(), Vec::new());
        };

        let borrowed = BorrowedSendParams::new(&params);
        let mut raw_result = CanopySendResult::default();
        let return_code = unsafe { callback(self.parent_ctx, borrowed.as_raw(), &mut raw_result) };
        let mut result = copy_send_result(&raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    fn post(&self, params: PostParams)
    {
        let Some(callback) = self.post else {
            return;
        };

        let borrowed = BorrowedPostParams::new(&params);
        unsafe { callback(self.parent_ctx, borrowed.as_raw()) };
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult
    {
        let Some(callback) = self.try_cast else {
            return StandardResult::new(error_codes::ZONE_NOT_FOUND(), Vec::new());
        };

        let borrowed = BorrowedTryCastParams::new(&params);
        let mut raw_result = CanopyStandardResult::default();
        let return_code = unsafe { callback(self.parent_ctx, borrowed.as_raw(), &mut raw_result) };
        let mut result = copy_standard_result(&raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult
    {
        let Some(callback) = self.add_ref else {
            return StandardResult::new(error_codes::ZONE_NOT_FOUND(), Vec::new());
        };

        let borrowed = BorrowedAddRefParams::new(&params);
        let mut raw_result = CanopyStandardResult::default();
        let return_code = unsafe { callback(self.parent_ctx, borrowed.as_raw(), &mut raw_result) };
        let mut result = copy_standard_result(&raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    fn release(&self, params: ReleaseParams) -> StandardResult
    {
        let Some(callback) = self.release else {
            return StandardResult::new(error_codes::ZONE_NOT_FOUND(), Vec::new());
        };

        let borrowed = BorrowedReleaseParams::new(&params);
        let mut raw_result = CanopyStandardResult::default();
        let return_code = unsafe { callback(self.parent_ctx, borrowed.as_raw(), &mut raw_result) };
        let mut result = copy_standard_result(&raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    fn object_released(&self, params: ObjectReleasedParams)
    {
        let Some(callback) = self.object_released else {
            return;
        };

        let borrowed = BorrowedObjectReleasedParams::new(&params);
        unsafe { callback(self.parent_ctx, borrowed.as_raw()) };
    }

    fn transport_down(&self, params: TransportDownParams)
    {
        let Some(callback) = self.transport_down else {
            return;
        };

        let borrowed = BorrowedTransportDownParams::new(&params);
        unsafe { callback(self.parent_ctx, borrowed.as_raw()) };
    }

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult
    {
        let Some(callback) = self.get_new_zone_id else {
            return NewZoneIdResult::new(error_codes::ZONE_NOT_FOUND(), Zone::default(), Vec::new());
        };

        let borrowed = BorrowedGetNewZoneIdParams::new(&params);
        let mut raw_result = CanopyNewZoneIdResult::default();
        let return_code = unsafe { callback(self.parent_ctx, borrowed.as_raw(), &mut raw_result) };
        let mut result = copy_new_zone_id_result(&raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }
}

#[cfg(test)]
mod tests
{
    use super::*;
    use canopy_rpc::{AddressType, DefaultValues, InterfaceOrdinal, Method, Object, ZoneAddressArgs};
    use std::collections::HashMap;

    #[derive(Default)]
    struct ParentCallbackState
    {
        observed_interface_id: u64,
        observed_method_id: u64,
        out_buf: Vec<u8>,
        out_payload: Vec<u8>,
    }

    unsafe extern "C" fn test_parent_send(
        parent_ctx: CanopyParentContext,
        params: *const CanopySendParams,
        result: *mut CanopySendResult,
    ) -> i32
    {
        let state = unsafe { &mut *(parent_ctx as *mut ParentCallbackState) };
        let params = unsafe { &*params };
        let result = unsafe { &mut *result };

        state.observed_interface_id = params.interface_id;
        state.observed_method_id = params.method_id;
        state.out_buf = vec![5, 4, 3];
        state.out_payload = vec![2, 1];

        let entries = vec![CanopyBackChannelEntry {
            type_id: 77,
            payload: CanopyConstByteBuffer::from_slice(&state.out_payload),
        }];
        let entries = Box::leak(entries.into_boxed_slice());

        result.error_code = 321;
        result.out_buf = CanopyByteBuffer {
            data: state.out_buf.as_mut_ptr(),
            size: state.out_buf.len(),
        };
        result.out_back_channel = CanopyMutBackChannelSpan {
            data: entries.as_mut_ptr(),
            size: entries.len(),
        };

        321
    }

    fn sample_zone_address() -> ZoneAddress
    {
        ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Ipv4,
            8080,
            vec![127, 0, 0, 1],
            32,
            7,
            16,
            9,
            vec![],
        ))
        .expect("sample zone address should be valid")
    }

    #[derive(Default)]
    struct TestAllocator
    {
        allocations: HashMap<usize, Box<[u8]>>,
    }

    unsafe extern "C" fn test_alloc(allocator_ctx: *mut c_void, size: usize) -> CanopyByteBuffer
    {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        let mut data = vec![0u8; size].into_boxed_slice();
        let ptr = data.as_mut_ptr();
        allocator.allocations.insert(ptr as usize, data);
        CanopyByteBuffer { data: ptr, size }
    }

    unsafe extern "C" fn test_free(allocator_ctx: *mut c_void, data: *mut u8, _size: usize)
    {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        allocator.allocations.remove(&(data as usize));
    }

    #[test]
    fn borrowed_send_params_preserve_runtime_values()
    {
        let zone = Zone::new(sample_zone_address());
        let remote = zone.with_object(Object::new(9)).expect("with_object should succeed");
        let params = SendParams {
            protocol_version: 3,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 44,
            caller_zone_id: zone,
            remote_object_id: remote,
            interface_id: InterfaceOrdinal::new(5),
            method_id: Method::new(6),
            in_data: vec![1, 2, 3],
            in_back_channel: vec![BackChannelEntry {
                type_id: 99,
                payload: vec![4, 5],
            }],
        };

        let borrowed = BorrowedSendParams::new(&params);
        let raw = borrowed.as_raw();

        assert_eq!(raw.protocol_version, 3);
        assert_eq!(raw.encoding_type, Encoding::ProtocolBuffers as u64);
        assert_eq!(raw.interface_id, 5);
        assert_eq!(raw.method_id, 6);
        assert_eq!(raw.in_data.size, 3);
        assert_eq!(raw.in_back_channel.size, 1);
    }

    #[test]
    fn copy_standard_result_copies_owned_data_out()
    {
        let payload = [7u8, 8u8];
        let entries = [CanopyBackChannelEntry {
            type_id: 12,
            payload: CanopyConstByteBuffer {
                data: payload.as_ptr(),
                size: payload.len(),
            },
        }];
        let raw = CanopyStandardResult {
            error_code: -5,
            out_back_channel: CanopyMutBackChannelSpan {
                data: entries.as_ptr() as *mut CanopyBackChannelEntry,
                size: entries.len(),
            },
        };

        let copied = copy_standard_result(&raw);
        assert_eq!(copied.error_code, -5);
        assert_eq!(copied.out_back_channel.len(), 1);
        assert_eq!(copied.out_back_channel[0].type_id, 12);
        assert_eq!(copied.out_back_channel[0].payload, vec![7, 8]);
    }

    #[test]
    fn parent_callbacks_send_routes_into_c_abi_function_pointer()
    {
        let zone = Zone::new(sample_zone_address());
        let remote = zone.with_object(Object::new(29)).expect("with_object should succeed");
        let mut parent_state = ParentCallbackState::default();
        let callbacks = ParentCallbacks {
            parent_ctx: (&mut parent_state as *mut ParentCallbackState).cast(),
            send: Some(test_parent_send),
            post: None,
            try_cast: None,
            add_ref: None,
            release: None,
            object_released: None,
            transport_down: None,
            get_new_zone_id: None,
        };

        let result = callbacks.send(SendParams {
            protocol_version: 3,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 101,
            caller_zone_id: zone,
            remote_object_id: remote,
            interface_id: InterfaceOrdinal::new(21),
            method_id: Method::new(22),
            in_data: vec![7, 6, 5],
            in_back_channel: Vec::new(),
        });

        assert_eq!(parent_state.observed_interface_id, 21);
        assert_eq!(parent_state.observed_method_id, 22);
        assert_eq!(result.error_code, 321);
        assert_eq!(result.out_buf, vec![5, 4, 3]);
        assert_eq!(result.out_back_channel.len(), 1);
        assert_eq!(result.out_back_channel[0].type_id, 77);
        assert_eq!(result.out_back_channel[0].payload, vec![2, 1]);
    }

    #[test]
    fn write_send_result_allocates_and_frees_with_allocator()
    {
        let mut allocator_state = TestAllocator::default();
        let allocator = CanopyAllocatorVtable {
            allocator_ctx: (&mut allocator_state as *mut TestAllocator).cast(),
            alloc: Some(test_alloc),
            free: Some(test_free),
        };
        let value = SendResult::new(
            17,
            vec![1, 2, 3],
            vec![BackChannelEntry {
                type_id: 44,
                payload: vec![9, 8],
            }],
        );
        let mut raw = CanopySendResult::default();

        write_send_result(&allocator, &value, &mut raw).expect("write_send_result should succeed");
        assert_eq!(raw.error_code, 17);
        assert_eq!(raw.out_buf.size, 3);
        assert_eq!(raw.out_back_channel.size, 1);
        assert_eq!(allocator_state.allocations.len(), 3);

        let copied = copy_send_result(&raw);
        assert_eq!(copied, value);

        free_send_result(&allocator, &mut raw);
        assert!(allocator_state.allocations.is_empty());
        assert_eq!(raw.out_buf, CanopyByteBuffer::default());
        assert_eq!(raw.out_back_channel, CanopyMutBackChannelSpan::default());
    }
}
