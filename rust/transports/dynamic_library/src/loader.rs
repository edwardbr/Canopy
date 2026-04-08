//! Safe parent-side dynamic-library loader for the shared `c_abi` surface.

use crate::ffi::{
    BorrowedAddRefParams, BorrowedGetNewZoneIdParams, BorrowedObjectReleasedParams,
    BorrowedPostParams, BorrowedReleaseParams, BorrowedSendParams, BorrowedTransportDownParams,
    BorrowedTryCastParams, CanopyAddRefParams, CanopyAllocatorVtable, CanopyChildContext,
    CanopyDllAddRefFn, CanopyDllDestroyFn, CanopyDllGetNewZoneIdFn, CanopyDllInitFn,
    CanopyDllInitParams, CanopyDllObjectReleasedFn, CanopyDllPostFn, CanopyDllReleaseFn,
    CanopyDllSendFn, CanopyDllTransportDownFn, CanopyDllTryCastFn, CanopyNewZoneIdResult,
    CanopyObjectReleasedParams, CanopyPostParams, CanopyReleaseParams, CanopySendResult,
    CanopyStandardResult, CanopyTransportDownParams, CanopyTryCastParams, copy_new_zone_id_result,
    copy_remote_object, copy_send_result, copy_standard_result, free_new_zone_id_result,
    free_remote_object, free_send_result, free_standard_result,
};
use crate::platform_ffi;
use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, RemoteObject, SendParams, SendResult, StandardResult, TransportDownParams,
    TryCastParams,
};

use std::ffi::CString;
use std::ffi::c_void;
use std::path::Path;

#[derive(Clone, Copy)]
pub struct DynamicLibraryExports {
    pub init: CanopyDllInitFn,
    pub destroy: CanopyDllDestroyFn,
    pub send: CanopyDllSendFn,
    pub post: CanopyDllPostFn,
    pub try_cast: CanopyDllTryCastFn,
    pub add_ref: CanopyDllAddRefFn,
    pub release: CanopyDllReleaseFn,
    pub object_released: CanopyDllObjectReleasedFn,
    pub transport_down: CanopyDllTransportDownFn,
    pub get_new_zone_id: CanopyDllGetNewZoneIdFn,
}

pub struct DynamicLibrary {
    handle: *mut c_void,
    exports: DynamicLibraryExports,
}

impl DynamicLibrary {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, String> {
        let path = path.as_ref().to_string_lossy().into_owned();
        let path = CString::new(path)
            .map_err(|_| "library path contains interior NUL byte".to_string())?;
        let init_name = CString::new("canopy_dll_init").expect("symbol name should be valid");
        let destroy_name = CString::new("canopy_dll_destroy").expect("symbol name should be valid");
        let send_name = CString::new("canopy_dll_send").expect("symbol name should be valid");
        let post_name = CString::new("canopy_dll_post").expect("symbol name should be valid");
        let try_cast_name =
            CString::new("canopy_dll_try_cast").expect("symbol name should be valid");
        let add_ref_name = CString::new("canopy_dll_add_ref").expect("symbol name should be valid");
        let release_name = CString::new("canopy_dll_release").expect("symbol name should be valid");
        let object_released_name =
            CString::new("canopy_dll_object_released").expect("symbol name should be valid");
        let transport_down_name =
            CString::new("canopy_dll_transport_down").expect("symbol name should be valid");
        let get_new_zone_id_name =
            CString::new("canopy_dll_get_new_zone_id").expect("symbol name should be valid");

        let handle = unsafe { platform_ffi::open_local_now(&path) };
        if handle.is_null() {
            return Err(unsafe { platform_ffi::last_error() });
        }

        let exports = (|| {
            Ok(DynamicLibraryExports {
                init: unsafe { platform_ffi::load_symbol(handle, &init_name)? },
                destroy: unsafe { platform_ffi::load_symbol(handle, &destroy_name)? },
                send: unsafe { platform_ffi::load_symbol(handle, &send_name)? },
                post: unsafe { platform_ffi::load_symbol(handle, &post_name)? },
                try_cast: unsafe { platform_ffi::load_symbol(handle, &try_cast_name)? },
                add_ref: unsafe { platform_ffi::load_symbol(handle, &add_ref_name)? },
                release: unsafe { platform_ffi::load_symbol(handle, &release_name)? },
                object_released: unsafe {
                    platform_ffi::load_symbol(handle, &object_released_name)?
                },
                transport_down: unsafe { platform_ffi::load_symbol(handle, &transport_down_name)? },
                get_new_zone_id: unsafe {
                    platform_ffi::load_symbol(handle, &get_new_zone_id_name)?
                },
            })
        })();

        match exports {
            Ok(exports) => Ok(Self { handle, exports }),
            Err(error) => {
                unsafe { platform_ffi::close(handle) };
                Err(error)
            }
        }
    }

    pub fn exports(&self) -> &DynamicLibraryExports {
        &self.exports
    }

    pub fn init_child(self, params: &mut CanopyDllInitParams) -> Result<LoadedChild, i32> {
        let error_code = unsafe { (self.exports.init)(params as *mut CanopyDllInitParams) };
        if error_code != error_codes::OK() {
            return Err(error_code);
        }

        let output_obj = copy_remote_object(params.output_obj);
        free_remote_object(&params.allocator, &mut params.output_obj);

        let child = LoadedChild {
            library: Some(self),
            child_ctx: params.child_ctx,
            allocator: params.allocator,
            output_obj,
        };

        params.child_ctx = std::ptr::null_mut();
        Ok(child)
    }
}

impl Drop for DynamicLibrary {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { platform_ffi::close(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

pub struct LoadedChild {
    library: Option<DynamicLibrary>,
    child_ctx: CanopyChildContext,
    allocator: CanopyAllocatorVtable,
    output_obj: RemoteObject,
}

impl LoadedChild {
    pub fn output_obj(&self) -> &RemoteObject {
        &self.output_obj
    }

    pub fn allocator(&self) -> &CanopyAllocatorVtable {
        &self.allocator
    }

    pub fn send(&self, params: &SendParams) -> SendResult {
        let borrowed = BorrowedSendParams::new(params);
        let mut raw_result = CanopySendResult::default();
        let return_code = unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .send)(self.child_ctx, borrowed.as_raw(), &mut raw_result)
        };
        let mut result = copy_send_result(&raw_result);
        free_send_result(&self.allocator, &mut raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    pub fn post(&self, params: &PostParams) {
        let borrowed = BorrowedPostParams::new(params);
        unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .post)(self.child_ctx, borrowed.as_raw() as *const CanopyPostParams)
        };
    }

    pub fn try_cast(&self, params: &TryCastParams) -> StandardResult {
        let borrowed = BorrowedTryCastParams::new(params);
        let mut raw_result = CanopyStandardResult::default();
        let return_code = unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .try_cast)(
                self.child_ctx,
                borrowed.as_raw() as *const CanopyTryCastParams,
                &mut raw_result,
            )
        };
        let mut result = copy_standard_result(&raw_result);
        free_standard_result(&self.allocator, &mut raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    pub fn add_ref(&self, params: &AddRefParams) -> StandardResult {
        let borrowed = BorrowedAddRefParams::new(params);
        let mut raw_result = CanopyStandardResult::default();
        let return_code = unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .add_ref)(
                self.child_ctx,
                borrowed.as_raw() as *const CanopyAddRefParams,
                &mut raw_result,
            )
        };
        let mut result = copy_standard_result(&raw_result);
        free_standard_result(&self.allocator, &mut raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    pub fn release(&self, params: &ReleaseParams) -> StandardResult {
        let borrowed = BorrowedReleaseParams::new(params);
        let mut raw_result = CanopyStandardResult::default();
        let return_code = unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .release)(
                self.child_ctx,
                borrowed.as_raw() as *const CanopyReleaseParams,
                &mut raw_result,
            )
        };
        let mut result = copy_standard_result(&raw_result);
        free_standard_result(&self.allocator, &mut raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    pub fn object_released(&self, params: &ObjectReleasedParams) {
        let borrowed = BorrowedObjectReleasedParams::new(params);
        unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .object_released)(
                self.child_ctx,
                borrowed.as_raw() as *const CanopyObjectReleasedParams,
            )
        };
    }

    pub fn transport_down(&self, params: &TransportDownParams) {
        let borrowed = BorrowedTransportDownParams::new(params);
        unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .transport_down)(
                self.child_ctx,
                borrowed.as_raw() as *const CanopyTransportDownParams,
            )
        };
    }

    pub fn get_new_zone_id(&self, params: &GetNewZoneIdParams) -> NewZoneIdResult {
        let borrowed = BorrowedGetNewZoneIdParams::new(params);
        let mut raw_result = CanopyNewZoneIdResult::default();
        let return_code = unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .get_new_zone_id)(self.child_ctx, borrowed.as_raw(), &mut raw_result)
        };
        let mut result = copy_new_zone_id_result(&raw_result);
        free_new_zone_id_result(&self.allocator, &mut raw_result);
        if result.error_code == error_codes::OK() && return_code != error_codes::OK() {
            result.error_code = return_code;
        }
        result
    }

    pub fn destroy(&mut self) {
        if self.child_ctx.is_null() {
            return;
        }

        unsafe {
            (self
                .library
                .as_ref()
                .expect("library should be present")
                .exports
                .destroy)(self.child_ctx);
        }
        self.child_ctx = std::ptr::null_mut();
    }
}

impl Drop for LoadedChild {
    fn drop(&mut self) {
        self.destroy();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::{
        CanopyByteBuffer, CanopyConnectionSettings, CanopyConstByteBuffer, CanopyDllInitParams,
        CanopyMutBackChannelSpan, borrow_remote_object, borrow_zone,
    };
    use canopy_rpc::{
        AddressType, DefaultValues, Encoding, InterfaceOrdinal, Method, Object, Zone, ZoneAddress,
        ZoneAddressArgs,
    };
    use std::collections::HashMap;

    #[derive(Default)]
    struct AllocatorState {
        allocations: HashMap<usize, Box<[u8]>>,
    }

    #[derive(Default)]
    struct ParentState {
        send_call_count: usize,
        zone_id_call_count: usize,
        observed_interface_id: u64,
        observed_method_id: u64,
        observed_payload: Vec<u8>,
        out_buf: Vec<u8>,
        zone_blob: Vec<u8>,
    }

    unsafe extern "C" fn test_alloc(allocator_ctx: *mut c_void, size: usize) -> CanopyByteBuffer {
        let allocator = unsafe { &mut *(allocator_ctx as *mut AllocatorState) };
        let mut data = vec![0u8; size].into_boxed_slice();
        let ptr = data.as_mut_ptr();
        allocator.allocations.insert(ptr as usize, data);
        CanopyByteBuffer { data: ptr, size }
    }

    unsafe extern "C" fn test_free(allocator_ctx: *mut c_void, data: *mut u8, _size: usize) {
        let allocator = unsafe { &mut *(allocator_ctx as *mut AllocatorState) };
        allocator.allocations.remove(&(data as usize));
    }

    unsafe extern "C" fn parent_send(
        parent_ctx: *mut c_void,
        params: *const crate::ffi::CanopySendParams,
        result: *mut crate::ffi::CanopySendResult,
    ) -> i32 {
        let state = unsafe { &mut *(parent_ctx as *mut ParentState) };
        let params = unsafe { &*params };
        let result = unsafe { &mut *result };

        state.send_call_count += 1;
        state.observed_interface_id = params.interface_id;
        state.observed_method_id = params.method_id;
        state.observed_payload = params.in_data.as_slice().to_vec();
        state.out_buf = b"parent-ok".to_vec();

        result.error_code = error_codes::OK();
        result.out_buf = CanopyByteBuffer {
            data: state.out_buf.as_mut_ptr(),
            size: state.out_buf.len(),
        };
        result.out_back_channel = CanopyMutBackChannelSpan::default();
        result.error_code
    }

    unsafe extern "C" fn parent_get_new_zone_id(
        parent_ctx: *mut c_void,
        _params: *const crate::ffi::CanopyGetNewZoneIdParams,
        result: *mut crate::ffi::CanopyNewZoneIdResult,
    ) -> i32 {
        let state = unsafe { &mut *(parent_ctx as *mut ParentState) };
        let result = unsafe { &mut *result };
        let zone = sample_zone();
        state.zone_id_call_count += 1;
        state.zone_blob = zone.get_address().get_blob().to_vec();

        result.error_code = error_codes::OK();
        result.zone_id = crate::ffi::CanopyZone {
            address: crate::ffi::CanopyZoneAddress {
                blob: CanopyConstByteBuffer::from_slice(&state.zone_blob),
            },
        };
        result.out_back_channel = CanopyMutBackChannelSpan::default();
        result.error_code
    }

    fn sample_zone() -> Zone {
        Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Ipv4,
                31337,
                vec![127, 0, 0, 1],
                32,
                88,
                16,
                0,
                vec![],
            ))
            .expect("sample zone address should be valid"),
        )
    }

    fn sample_remote_object() -> RemoteObject {
        sample_zone()
            .with_object(Object::new(100))
            .expect("with_object should succeed")
    }

    fn stats_request(output_obj: &RemoteObject) -> SendParams {
        SendParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 99,
            caller_zone_id: sample_zone(),
            remote_object_id: output_obj.clone(),
            interface_id: InterfaceOrdinal::new(7),
            method_id: Method::new(8),
            in_data: b"stats".to_vec(),
            in_back_channel: Vec::new(),
        }
    }

    #[test]
    fn rust_parent_can_load_rust_child_dll_when_path_is_provided() {
        let Ok(path) = std::env::var("CANOPY_RUST_TEST_DLL_PATH") else {
            return;
        };

        let mut allocator_state = AllocatorState::default();
        let mut parent_state = ParentState::default();
        let parent_zone = sample_zone();
        let child_zone = sample_zone();
        let input_descr = CanopyConnectionSettings {
            inbound_interface_id: 1,
            outbound_interface_id: 2,
            remote_object_id: borrow_remote_object(&sample_remote_object()),
        };

        let mut init_params = CanopyDllInitParams {
            name: c"rust-parent".as_ptr(),
            parent_zone: borrow_zone(&parent_zone),
            child_zone: borrow_zone(&child_zone),
            input_descr: &input_descr,
            parent_ctx: (&mut parent_state as *mut ParentState).cast(),
            allocator: CanopyAllocatorVtable {
                allocator_ctx: (&mut allocator_state as *mut AllocatorState).cast(),
                alloc: Some(test_alloc),
                free: Some(test_free),
            },
            parent_send: Some(parent_send),
            parent_get_new_zone_id: Some(parent_get_new_zone_id),
            ..CanopyDllInitParams::default()
        };

        let library = DynamicLibrary::load(path).expect("library should load");
        let mut child = library
            .init_child(&mut init_params)
            .expect("child should initialize");

        let send_result = child.send(&SendParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 1,
            caller_zone_id: parent_zone,
            remote_object_id: child.output_obj().clone(),
            interface_id: InterfaceOrdinal::new(1),
            method_id: Method::new(2),
            in_data: b"ping".to_vec(),
            in_back_channel: Vec::new(),
        });
        assert_eq!(send_result.error_code, error_codes::OK());
        assert_eq!(send_result.out_buf, b"rust-child:ping".to_vec());

        let callback_send_result = child.send(&SendParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 2,
            caller_zone_id: sample_zone(),
            remote_object_id: child.output_obj().clone(),
            interface_id: InterfaceOrdinal::new(3),
            method_id: Method::new(4),
            in_data: b"call-parent-send".to_vec(),
            in_back_channel: Vec::new(),
        });
        assert_eq!(callback_send_result.error_code, error_codes::OK());
        assert_eq!(callback_send_result.out_buf, b"parent-ok".to_vec());
        assert_eq!(parent_state.send_call_count, 1);
        assert_eq!(parent_state.observed_interface_id, 41);
        assert_eq!(parent_state.observed_method_id, 42);
        assert_eq!(parent_state.observed_payload, b"from-rust-child".to_vec());

        let zone_id_result = child.send(&SendParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 3,
            caller_zone_id: sample_zone(),
            remote_object_id: child.output_obj().clone(),
            interface_id: InterfaceOrdinal::new(5),
            method_id: Method::new(6),
            in_data: b"call-parent-get-new-zone-id".to_vec(),
            in_back_channel: Vec::new(),
        });
        assert_eq!(zone_id_result.error_code, error_codes::OK());
        assert_eq!(zone_id_result.out_buf, b"zone:88".to_vec());
        assert_eq!(parent_state.zone_id_call_count, 1);

        child.destroy();
    }

    #[test]
    fn rust_parent_loader_exercises_remaining_marshaller_methods_when_path_is_provided() {
        let Ok(path) = std::env::var("CANOPY_RUST_TEST_DLL_PATH") else {
            return;
        };

        let mut allocator_state = AllocatorState::default();
        let parent_zone = sample_zone();
        let child_zone = sample_zone();
        let input_descr = CanopyConnectionSettings {
            inbound_interface_id: 1,
            outbound_interface_id: 2,
            remote_object_id: borrow_remote_object(&sample_remote_object()),
        };

        let mut init_params = CanopyDllInitParams {
            name: c"rust-parent".as_ptr(),
            parent_zone: borrow_zone(&parent_zone),
            child_zone: borrow_zone(&child_zone),
            input_descr: &input_descr,
            allocator: CanopyAllocatorVtable {
                allocator_ctx: (&mut allocator_state as *mut AllocatorState).cast(),
                alloc: Some(test_alloc),
                free: Some(test_free),
            },
            ..CanopyDllInitParams::default()
        };

        let library = DynamicLibrary::load(path).expect("library should load");
        let mut child = library
            .init_child(&mut init_params)
            .expect("child should initialize");
        let output_obj = child.output_obj().clone();

        let try_cast_result = child.try_cast(&TryCastParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            caller_zone_id: sample_zone(),
            remote_object_id: output_obj.clone(),
            interface_id: InterfaceOrdinal::new(21),
            in_back_channel: Vec::new(),
        });
        assert_eq!(try_cast_result.error_code, error_codes::OK());

        child.post(&PostParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 10,
            caller_zone_id: sample_zone(),
            remote_object_id: output_obj.clone(),
            interface_id: InterfaceOrdinal::new(11),
            method_id: Method::new(12),
            in_data: b"post".to_vec(),
            in_back_channel: Vec::new(),
        });

        let add_ref_result = child.add_ref(&AddRefParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            remote_object_id: output_obj.clone(),
            caller_zone_id: sample_zone(),
            requesting_zone_id: sample_zone(),
            build_out_param_channel: canopy_rpc::AddRefOptions::NORMAL,
            in_back_channel: Vec::new(),
        });
        assert_eq!(add_ref_result.error_code, error_codes::OK());

        let release_result = child.release(&ReleaseParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            remote_object_id: output_obj.clone(),
            caller_zone_id: sample_zone(),
            options: canopy_rpc::ReleaseOptions::NORMAL,
            in_back_channel: Vec::new(),
        });
        assert_eq!(release_result.error_code, error_codes::OK());

        child.object_released(&ObjectReleasedParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            remote_object_id: output_obj.clone(),
            caller_zone_id: sample_zone(),
            in_back_channel: Vec::new(),
        });

        child.transport_down(&TransportDownParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            destination_zone_id: sample_zone(),
            caller_zone_id: sample_zone(),
            in_back_channel: Vec::new(),
        });

        let get_new_zone_id_result = child.get_new_zone_id(&GetNewZoneIdParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            in_back_channel: Vec::new(),
        });
        assert_eq!(get_new_zone_id_result.error_code, error_codes::OK());
        assert_eq!(get_new_zone_id_result.zone_id.get_subnet(), 7);

        let stats_result = child.send(&stats_request(&output_obj));
        assert_eq!(stats_result.error_code, error_codes::OK());
        assert_eq!(
            String::from_utf8(stats_result.out_buf).expect("stats should be utf8"),
            "post=1;add_ref=1;release=1;object_released=1;transport_down=1"
        );

        child.destroy();
    }
}
