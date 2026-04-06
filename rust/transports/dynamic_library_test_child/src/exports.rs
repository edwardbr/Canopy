//! Exported `canopy_dll_*` symbols for the handwritten Rust child DLL.
//!
//! This module is the only FFI boundary for the test child crate. Keep raw
//! pointer validation and other unsafety here.

use crate::{TestChildMarshaller, sample_output_object};
use canopy_rpc::internal::error_codes;
use canopy_transport_dynamic_library as dll;

fn mut_ptr<'a, T>(ptr: *mut T) -> Option<&'a mut T>
{
    unsafe { ptr.as_mut() }
}

fn const_ptr<'a, T>(ptr: *const T) -> Option<&'a T>
{
    unsafe { ptr.as_ref() }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_init(params: *mut dll::CanopyDllInitParams) -> i32
{
    let Some(params) = mut_ptr(params) else {
        return error_codes::INVALID_DATA();
    };

    dll::dll_init::<TestChildMarshaller, _>(params, |_parent_transport, _input_descr| {
        Ok((TestChildMarshaller::new(_parent_transport.clone()), sample_output_object()))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_destroy(child_ctx: dll::CanopyChildContext)
{
    dll::dll_destroy::<TestChildMarshaller>(child_ctx);
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_send(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopySendParams,
    result: *mut dll::CanopySendResult,
) -> i32
{
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };

    dll::dll_send::<TestChildMarshaller>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_post(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyPostParams,
)
{
    let Some(params) = const_ptr(params) else {
        return;
    };

    dll::dll_post::<TestChildMarshaller>(child_ctx, params);
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_try_cast(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyTryCastParams,
    result: *mut dll::CanopyStandardResult,
) -> i32
{
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };

    dll::dll_try_cast::<TestChildMarshaller>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_add_ref(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyAddRefParams,
    result: *mut dll::CanopyStandardResult,
) -> i32
{
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };

    dll::dll_add_ref::<TestChildMarshaller>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_release(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyReleaseParams,
    result: *mut dll::CanopyStandardResult,
) -> i32
{
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };

    dll::dll_release::<TestChildMarshaller>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_object_released(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyObjectReleasedParams,
)
{
    let Some(params) = const_ptr(params) else {
        return;
    };

    dll::dll_object_released::<TestChildMarshaller>(child_ctx, params);
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_transport_down(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyTransportDownParams,
)
{
    let Some(params) = const_ptr(params) else {
        return;
    };

    dll::dll_transport_down::<TestChildMarshaller>(child_ctx, params);
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_get_new_zone_id(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyGetNewZoneIdParams,
    result: *mut dll::CanopyNewZoneIdResult,
) -> i32
{
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };

    dll::dll_get_new_zone_id::<TestChildMarshaller>(child_ctx, params, result)
}

#[cfg(test)]
mod tests
{
    use super::*;
    use canopy_rpc::{AddressType, DefaultValues, Encoding, InterfaceOrdinal, Method, Object, Zone, ZoneAddress, ZoneAddressArgs};
    use std::collections::HashMap;

    #[derive(Default)]
    struct TestAllocator
    {
        allocations: HashMap<usize, Box<[u8]>>,
    }

    unsafe extern "C" fn test_alloc(
        allocator_ctx: *mut std::ffi::c_void,
        size: usize,
    ) -> dll::CanopyByteBuffer
    {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        let mut data = vec![0u8; size].into_boxed_slice();
        let ptr = data.as_mut_ptr();
        allocator.allocations.insert(ptr as usize, data);
        dll::CanopyByteBuffer { data: ptr, size }
    }

    unsafe extern "C" fn test_free(allocator_ctx: *mut std::ffi::c_void, data: *mut u8, _size: usize)
    {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        allocator.allocations.remove(&(data as usize));
    }

    #[test]
    fn exported_init_send_destroy_round_trip()
    {
        let mut allocator_state = TestAllocator::default();
        let mut init = dll::CanopyDllInitParams {
            allocator: dll::CanopyAllocatorVtable {
                allocator_ctx: (&mut allocator_state as *mut TestAllocator).cast(),
                alloc: Some(test_alloc),
                free: Some(test_free),
            },
            ..dll::CanopyDllInitParams::default()
        };

        let init_code = canopy_dll_init(&mut init);
        assert_eq!(init_code, error_codes::OK());
        assert!(!init.child_ctx.is_null());

        let zone_address = ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Ipv4,
            31337,
            vec![127, 0, 0, 1],
            32,
            7,
            16,
            0,
            vec![],
        ))
        .expect("sample zone address should be valid");
        let zone = Zone::new(zone_address);
        let remote = zone.with_object(Object::new(100)).expect("with_object should succeed");
        let input = b"ping";
        let params = dll::CanopySendParams {
            protocol_version: 3,
            encoding_type: Encoding::ProtocolBuffers as u64,
            tag: 55,
            caller_zone_id: dll::borrow_zone(&zone),
            remote_object_id: dll::borrow_remote_object(&remote),
            interface_id: InterfaceOrdinal::new(1).get_val(),
            method_id: Method::new(2).get_val(),
            in_data: dll::CanopyConstByteBuffer::from_slice(input),
            in_back_channel: dll::CanopyBackChannelSpan::default(),
        };
        let mut result = dll::CanopySendResult::default();

        let send_code = canopy_dll_send(init.child_ctx, &params, &mut result);
        assert_eq!(send_code, error_codes::OK());

        let copied = dll::copy_send_result(&result);
        assert_eq!(copied.out_buf, b"rust-child:ping".to_vec());

        dll::dll_free_send_result(&init.allocator, &mut result);
        canopy_dll_destroy(init.child_ctx);
    }
}
