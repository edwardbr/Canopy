//! Generic child-side entrypoint façade for `canopy_dll_*`-style exports.
//!
//! These helpers are not exported symbols yet. They provide the logic that
//! real `extern "C"` entrypoints can delegate to once the Rust child runtime
//! is ready to expose them directly.

use crate::context::init_child_zone;
use crate::ffi::{
    CanopyAddRefParams, CanopyChildContext, CanopyDllInitParams, CanopyGetNewZoneIdParams, CanopyNewZoneIdResult,
    CanopyObjectReleasedParams, CanopyPostParams, CanopyReleaseParams, CanopySendParams, CanopySendResult,
    CanopyStandardResult, CanopyTransportDownParams, CanopyTryCastParams, box_child_context, destroy_child_context,
    free_new_zone_id_result, free_send_result, free_standard_result, with_child_context, write_new_zone_id_result,
    write_send_result, write_standard_result,
};
use canopy_rpc::internal::error_codes;
use canopy_rpc::{IMarshaller, RemoteObject};
use canopy_rpc::rpc_types::ConnectionSettings;

pub fn dll_init<M, F>(
    params: &mut CanopyDllInitParams,
    factory: F,
) -> i32
where
    M: IMarshaller,
    F: FnOnce(
        &crate::ParentTransportAdapter<crate::ParentCallbacks>,
        Option<&ConnectionSettings>,
    ) -> Result<(M, RemoteObject), i32>,
{
    match init_child_zone(params, factory) {
        Ok(result) => {
            params.child_ctx = box_child_context(result.context);
            error_codes::OK()
        }
        Err(error_code) => error_code,
    }
}

pub fn dll_destroy<M>(child_ctx: CanopyChildContext)
where
    M: IMarshaller,
{
    let _ = destroy_child_context::<M>(child_ctx);
}

pub fn dll_send<M>(
    child_ctx: CanopyChildContext,
    params: &CanopySendParams,
    result: &mut CanopySendResult,
) -> i32
where
    M: IMarshaller,
{
    let Some(ret) = with_child_context::<M, _>(child_ctx, |context| {
        (context.child_transport().send(params), *context.allocator())
    }) else {
        *result = CanopySendResult::default();
        result.error_code = error_codes::ZONE_NOT_FOUND();
        return result.error_code;
    };
    let (runtime_result, allocator) = ret;

    if let Err(error_code) = write_send_result(&allocator, &runtime_result, result) {
        *result = CanopySendResult::default();
        result.error_code = error_code;
        return error_code;
    }

    runtime_result.error_code
}

pub fn dll_post<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyPostParams,
) where
    M: IMarshaller,
{
    if let Some(ret) = with_child_context::<M, _>(child_ctx, |context| context.child_transport().post(params)) {
        let _ = ret;
    }
}

pub fn dll_try_cast<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyTryCastParams,
    result: &mut CanopyStandardResult,
) -> i32
where
    M: IMarshaller,
{
    let Some(ret) = with_child_context::<M, _>(child_ctx, |context| {
        (context.child_transport().try_cast(params), *context.allocator())
    }) else {
        *result = CanopyStandardResult::default();
        result.error_code = error_codes::ZONE_NOT_FOUND();
        return result.error_code;
    };
    let (runtime_result, allocator) = ret;

    if let Err(error_code) = write_standard_result(&allocator, &runtime_result, result) {
        *result = CanopyStandardResult::default();
        result.error_code = error_code;
        return error_code;
    }

    runtime_result.error_code
}

pub fn dll_add_ref<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyAddRefParams,
    result: &mut CanopyStandardResult,
) -> i32
where
    M: IMarshaller,
{
    let Some(ret) = with_child_context::<M, _>(child_ctx, |context| {
        (context.child_transport().add_ref(params), *context.allocator())
    }) else {
        *result = CanopyStandardResult::default();
        result.error_code = error_codes::ZONE_NOT_FOUND();
        return result.error_code;
    };
    let (runtime_result, allocator) = ret;

    if let Err(error_code) = write_standard_result(&allocator, &runtime_result, result) {
        *result = CanopyStandardResult::default();
        result.error_code = error_code;
        return error_code;
    }

    runtime_result.error_code
}

pub fn dll_release<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyReleaseParams,
    result: &mut CanopyStandardResult,
) -> i32
where
    M: IMarshaller,
{
    let Some(ret) = with_child_context::<M, _>(child_ctx, |context| {
        (context.child_transport().release(params), *context.allocator())
    }) else {
        *result = CanopyStandardResult::default();
        result.error_code = error_codes::ZONE_NOT_FOUND();
        return result.error_code;
    };
    let (runtime_result, allocator) = ret;

    if let Err(error_code) = write_standard_result(&allocator, &runtime_result, result) {
        *result = CanopyStandardResult::default();
        result.error_code = error_code;
        return error_code;
    }

    runtime_result.error_code
}

pub fn dll_object_released<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyObjectReleasedParams,
) where
    M: IMarshaller,
{
    if let Some(ret) = with_child_context::<M, _>(child_ctx, |context| context.child_transport().object_released(params)) {
        let _ = ret;
    }
}

pub fn dll_transport_down<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyTransportDownParams,
) where
    M: IMarshaller,
{
    if let Some(ret) = with_child_context::<M, _>(child_ctx, |context| context.child_transport().transport_down(params)) {
        let _ = ret;
    }
}

pub fn dll_get_new_zone_id<M>(
    child_ctx: CanopyChildContext,
    params: &CanopyGetNewZoneIdParams,
    result: &mut CanopyNewZoneIdResult,
) -> i32
where
    M: IMarshaller,
{
    let Some(ret) =
        with_child_context::<M, _>(child_ctx, |context| (context.child_transport().get_new_zone_id(params), *context.allocator()))
    else {
        *result = CanopyNewZoneIdResult::default();
        result.error_code = error_codes::ZONE_NOT_FOUND();
        return result.error_code;
    };
    let (runtime_result, allocator) = ret;

    if let Err(error_code) = write_new_zone_id_result(&allocator, &runtime_result, result) {
        *result = CanopyNewZoneIdResult::default();
        result.error_code = error_code;
        return error_code;
    }

    runtime_result.error_code
}

pub fn dll_free_send_result(
    allocator: &crate::CanopyAllocatorVtable,
    result: &mut CanopySendResult,
)
{
    free_send_result(allocator, result);
}

pub fn dll_free_standard_result(
    allocator: &crate::CanopyAllocatorVtable,
    result: &mut CanopyStandardResult,
)
{
    free_standard_result(allocator, result);
}

pub fn dll_free_new_zone_id_result(
    allocator: &crate::CanopyAllocatorVtable,
    result: &mut CanopyNewZoneIdResult,
)
{
    free_new_zone_id_result(allocator, result);
}

#[cfg(test)]
mod tests
{
    use super::*;
    use crate::CanopyAllocatorVtable;
    use crate::CanopyBackChannelSpan;
    use crate::CanopyByteBuffer;
    use crate::CanopyConstByteBuffer;
    use crate::CanopySendParams;
    use crate::borrow_remote_object;
    use crate::borrow_zone;
    use canopy_rpc::internal::error_codes;
    use canopy_rpc::{
        AddRefParams, AddressType, DefaultValues, Encoding, GetNewZoneIdParams, Object, ObjectReleasedParams,
        PostParams, ReleaseParams, SendParams, StandardResult, TransportDownParams, TryCastParams, Zone, ZoneAddress,
        ZoneAddressArgs,
    };
    use std::collections::HashMap;

    #[derive(Default)]
    struct TestAllocator
    {
        allocations: HashMap<usize, Box<[u8]>>,
    }

    unsafe extern "C" fn test_alloc(allocator_ctx: *mut std::ffi::c_void, size: usize) -> CanopyByteBuffer
    {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        let mut data = vec![0u8; size].into_boxed_slice();
        let ptr = data.as_mut_ptr();
        allocator.allocations.insert(ptr as usize, data);
        CanopyByteBuffer { data: ptr, size }
    }

    unsafe extern "C" fn test_free(allocator_ctx: *mut std::ffi::c_void, data: *mut u8, _size: usize)
    {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        allocator.allocations.remove(&(data as usize));
    }

    #[derive(Default)]
    struct TestMarshaller;

    impl IMarshaller for TestMarshaller
    {
        fn send(&self, _params: SendParams) -> canopy_rpc::SendResult
        {
            canopy_rpc::SendResult::new(error_codes::OK(), vec![1, 2, 3], Vec::new())
        }

        fn post(&self, _params: PostParams) {}
        fn try_cast(&self, _params: TryCastParams) -> StandardResult { StandardResult::new(error_codes::OK(), Vec::new()) }
        fn add_ref(&self, _params: AddRefParams) -> StandardResult { StandardResult::new(error_codes::OK(), Vec::new()) }
        fn release(&self, _params: ReleaseParams) -> StandardResult { StandardResult::new(error_codes::OK(), Vec::new()) }
        fn object_released(&self, _params: ObjectReleasedParams) {}
        fn transport_down(&self, _params: TransportDownParams) {}
        fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> canopy_rpc::NewZoneIdResult
        {
            canopy_rpc::NewZoneIdResult::new(error_codes::OK(), Zone::default(), Vec::new())
        }
    }

    fn sample_remote_object() -> RemoteObject
    {
        let zone_address = ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Ipv4,
            8080,
            vec![127, 0, 0, 1],
            32,
            7,
            16,
            0,
            vec![],
        ))
        .expect("sample zone address should be valid");

        Zone::new(zone_address)
            .with_object(Object::new(42))
            .expect("with_object should succeed")
    }

    #[test]
    fn dll_init_and_send_round_trip_through_child_ctx()
    {
        let mut allocator_state = TestAllocator::default();
        let allocator = CanopyAllocatorVtable {
            allocator_ctx: (&mut allocator_state as *mut TestAllocator).cast(),
            alloc: Some(test_alloc),
            free: Some(test_free),
        };
        let mut init = CanopyDllInitParams {
            allocator,
            ..CanopyDllInitParams::default()
        };
        let output_obj = sample_remote_object();

        let init_code = dll_init::<TestMarshaller, _>(&mut init, |_parent, _input| Ok((TestMarshaller, output_obj)));
        assert_eq!(init_code, error_codes::OK());
        assert!(!init.child_ctx.is_null());

        let zone = Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Ipv4,
                8080,
                vec![127, 0, 0, 1],
                32,
                7,
                16,
                0,
                vec![],
            ))
            .expect("sample zone address should be valid"),
        );
        let remote = zone.with_object(Object::new(17)).expect("with_object should succeed");
        let raw_params = CanopySendParams {
            protocol_version: 3,
            encoding_type: Encoding::ProtocolBuffers as u64,
            tag: 1,
            caller_zone_id: borrow_zone(&zone),
            remote_object_id: borrow_remote_object(&remote),
            interface_id: 5,
            method_id: 6,
            in_data: CanopyConstByteBuffer::default(),
            in_back_channel: CanopyBackChannelSpan::default(),
        };

        let mut raw_result = CanopySendResult::default();
        let send_code = dll_send::<TestMarshaller>(init.child_ctx, &raw_params, &mut raw_result);
        assert_eq!(send_code, error_codes::OK());
        assert_eq!(raw_result.out_buf.size, 3);

        dll_free_send_result(&allocator, &mut raw_result);
        dll_destroy::<TestMarshaller>(init.child_ctx);
    }
}
