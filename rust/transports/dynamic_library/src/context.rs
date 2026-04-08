//! Safe child-side context and `init_child_zone` helper sketch.
//!
//! This mirrors the role of the C++ DLL-side `dll_context` and
//! `init_child_zone` helper without depending on the not-yet-ported Rust
//! service/transport stack.

use crate::adapter::{ChildTransportAdapter, ParentTransportAdapter};
use crate::ffi::{
    CanopyAllocatorVtable, CanopyDllInitParams, ParentCallbacks, copy_init_connection_settings,
    write_init_output_obj,
};
use canopy_rpc::rpc_types::ConnectionSettings;
use canopy_rpc::{IMarshaller, RemoteObject};

use std::sync::atomic::{AtomicBool, Ordering};

pub struct DllContext<M> {
    allocator: CanopyAllocatorVtable,
    parent_transport: ParentTransportAdapter<ParentCallbacks>,
    child_transport: ChildTransportAdapter<M>,
    input_descr: Option<ConnectionSettings>,
    output_obj: RemoteObject,
    destroyed: AtomicBool,
}

impl<M> DllContext<M>
where
    M: IMarshaller,
{
    pub fn new(
        allocator: CanopyAllocatorVtable,
        parent_transport: ParentTransportAdapter<ParentCallbacks>,
        child_transport: ChildTransportAdapter<M>,
        input_descr: Option<ConnectionSettings>,
        output_obj: RemoteObject,
    ) -> Self {
        Self {
            allocator,
            parent_transport,
            child_transport,
            input_descr,
            output_obj,
            destroyed: AtomicBool::new(false),
        }
    }

    pub fn parent_transport(&self) -> &ParentTransportAdapter<ParentCallbacks> {
        &self.parent_transport
    }

    pub fn allocator(&self) -> &CanopyAllocatorVtable {
        &self.allocator
    }

    pub fn child_transport(&self) -> &ChildTransportAdapter<M> {
        &self.child_transport
    }

    pub fn input_descr(&self) -> Option<&ConnectionSettings> {
        self.input_descr.as_ref()
    }

    pub fn output_obj(&self) -> &RemoteObject {
        &self.output_obj
    }

    pub fn is_destroyed(&self) -> bool {
        self.destroyed.load(Ordering::Acquire)
    }

    pub fn destroy(&self) -> bool {
        !self.destroyed.swap(true, Ordering::AcqRel)
    }
}

pub struct InitChildZoneResult<M> {
    pub context: DllContext<M>,
    pub output_obj: RemoteObject,
}

pub fn init_child_zone<M, F>(
    params: &mut CanopyDllInitParams,
    factory: F,
) -> Result<InitChildZoneResult<M>, i32>
where
    M: IMarshaller,
    F: FnOnce(
        &ParentTransportAdapter<ParentCallbacks>,
        Option<&ConnectionSettings>,
    ) -> Result<(M, RemoteObject), i32>,
{
    let parent_transport = ParentTransportAdapter::from_dll_init_params(params);
    let input_descr = copy_init_connection_settings(params);
    let (child_marshaller, output_obj) = factory(&parent_transport, input_descr.as_ref())?;
    let child_transport = ChildTransportAdapter::new(child_marshaller);
    write_init_output_obj(params, &output_obj)?;
    let context = DllContext::new(
        params.allocator,
        parent_transport,
        child_transport,
        input_descr,
        output_obj.clone(),
    );

    Ok(InitChildZoneResult {
        context,
        output_obj,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use canopy_rpc::internal::error_codes;
    use canopy_rpc::{
        AddRefParams, AddressType, DefaultValues, GetNewZoneIdParams, Object, ObjectReleasedParams,
        PostParams, ReleaseParams, SendParams, StandardResult, TransportDownParams, TryCastParams,
        Zone, ZoneAddress, ZoneAddressArgs,
    };
    use std::collections::HashMap;

    #[derive(Default)]
    struct NoopMarshaller;

    impl IMarshaller for NoopMarshaller {
        fn send(&self, _params: SendParams) -> canopy_rpc::SendResult {
            canopy_rpc::SendResult::new(error_codes::OK(), Vec::new(), Vec::new())
        }

        fn post(&self, _params: PostParams) {}

        fn try_cast(&self, _params: TryCastParams) -> StandardResult {
            StandardResult::new(error_codes::OK(), Vec::new())
        }

        fn add_ref(&self, _params: AddRefParams) -> StandardResult {
            StandardResult::new(error_codes::OK(), Vec::new())
        }

        fn release(&self, _params: ReleaseParams) -> StandardResult {
            StandardResult::new(error_codes::OK(), Vec::new())
        }

        fn object_released(&self, _params: ObjectReleasedParams) {}

        fn transport_down(&self, _params: TransportDownParams) {}

        fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> canopy_rpc::NewZoneIdResult {
            canopy_rpc::NewZoneIdResult::new(error_codes::OK(), Zone::default(), Vec::new())
        }
    }

    fn sample_remote_object() -> RemoteObject {
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

    #[derive(Default)]
    struct TestAllocator {
        allocations: HashMap<usize, Box<[u8]>>,
    }

    unsafe extern "C" fn test_alloc(
        allocator_ctx: *mut std::ffi::c_void,
        size: usize,
    ) -> crate::ffi::CanopyByteBuffer {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        let mut data = vec![0u8; size].into_boxed_slice();
        let ptr = data.as_mut_ptr();
        allocator.allocations.insert(ptr as usize, data);
        crate::ffi::CanopyByteBuffer { data: ptr, size }
    }

    unsafe extern "C" fn test_free(
        allocator_ctx: *mut std::ffi::c_void,
        data: *mut u8,
        _size: usize,
    ) {
        let allocator = unsafe { &mut *(allocator_ctx as *mut TestAllocator) };
        allocator.allocations.remove(&(data as usize));
    }

    #[test]
    fn init_child_zone_builds_context_and_output_descriptor() {
        let mut allocator = TestAllocator::default();
        let mut params = CanopyDllInitParams {
            allocator: crate::ffi::CanopyAllocatorVtable {
                allocator_ctx: (&mut allocator as *mut TestAllocator).cast(),
                alloc: Some(test_alloc),
                free: Some(test_free),
            },
            ..CanopyDllInitParams::default()
        };
        let expected_output = sample_remote_object();

        let result = init_child_zone(&mut params, |_parent_transport, input_descr| {
            assert!(input_descr.is_none());
            Ok((NoopMarshaller, expected_output.clone()))
        })
        .expect("init_child_zone should succeed");

        assert_eq!(result.output_obj, expected_output);
        assert_eq!(result.context.output_obj(), &expected_output);
        assert!(!result.context.is_destroyed());
        assert!(!params.output_obj.address.blob.data.is_null());
        assert_eq!(
            params.output_obj.address.blob.size,
            expected_output.get_address().get_blob().len()
        );
    }

    #[test]
    fn destroy_is_idempotent() {
        let mut allocator = TestAllocator::default();
        let mut params = CanopyDllInitParams {
            allocator: crate::ffi::CanopyAllocatorVtable {
                allocator_ctx: (&mut allocator as *mut TestAllocator).cast(),
                alloc: Some(test_alloc),
                free: Some(test_free),
            },
            ..CanopyDllInitParams::default()
        };
        let output = sample_remote_object();
        let result = init_child_zone(&mut params, |_parent_transport, _input_descr| {
            Ok((NoopMarshaller, output))
        })
        .expect("init_child_zone should succeed");

        assert!(result.context.destroy());
        assert!(result.context.is_destroyed());
        assert!(!result.context.destroy());
    }
}
