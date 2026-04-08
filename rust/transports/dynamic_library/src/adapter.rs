//! Safe transport adapter layer built on top of the raw FFI module.

use crate::ffi::{
    CanopyAddRefParams, CanopyDllInitParams, CanopyGetNewZoneIdParams, CanopyObjectReleasedParams,
    CanopyPostParams, CanopyReleaseParams, CanopySendParams, CanopyTransportDownParams,
    CanopyTryCastParams, ParentCallbacks, copy_add_ref_params, copy_get_new_zone_id_params,
    copy_object_released_params, copy_post_params, copy_release_params, copy_send_params,
    copy_transport_down_params, copy_try_cast_params,
};
use canopy_rpc::{
    AddRefParams, GetNewZoneIdParams, IMarshaller, NewZoneIdResult, ObjectReleasedParams,
    PostParams, ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams,
    TryCastParams,
};

/// Executor-neutral Rust-side sketch of the child DLL transport adapter.
///
/// This layer decodes raw C ABI request structs into `canopy-rpc` runtime
/// structs and then forwards them to an `IMarshaller` implementation.
pub struct ChildTransportAdapter<M> {
    marshaller: M,
}

impl<M> ChildTransportAdapter<M>
where
    M: IMarshaller,
{
    pub fn new(marshaller: M) -> Self {
        Self { marshaller }
    }

    pub fn send(&self, raw: &CanopySendParams) -> SendResult {
        match copy_send_params(raw) {
            Ok(params) => self.marshaller.send(params),
            Err(error_code) => SendResult::new(error_code, Vec::new(), Vec::new()),
        }
    }

    pub fn post(&self, raw: &CanopyPostParams) -> Result<(), i32> {
        match copy_post_params(raw) {
            Ok(params) => {
                self.marshaller.post(params);
                Ok(())
            }
            Err(error_code) => Err(error_code),
        }
    }

    pub fn try_cast(&self, raw: &CanopyTryCastParams) -> StandardResult {
        self.marshaller.try_cast(copy_try_cast_params(raw))
    }

    pub fn add_ref(&self, raw: &CanopyAddRefParams) -> StandardResult {
        self.marshaller.add_ref(copy_add_ref_params(raw))
    }

    pub fn release(&self, raw: &CanopyReleaseParams) -> StandardResult {
        self.marshaller.release(copy_release_params(raw))
    }

    pub fn object_released(&self, raw: &CanopyObjectReleasedParams) {
        self.marshaller
            .object_released(copy_object_released_params(raw));
    }

    pub fn transport_down(&self, raw: &CanopyTransportDownParams) {
        self.marshaller
            .transport_down(copy_transport_down_params(raw));
    }

    pub fn get_new_zone_id(&self, raw: &CanopyGetNewZoneIdParams) -> NewZoneIdResult {
        self.marshaller
            .get_new_zone_id(copy_get_new_zone_id_params(raw))
    }
}

/// Safe parent-side adaptor for the DLL zone's outbound view of its parent.
///
/// This mirrors the role of the C++ `parent_transport`: methods are expressed
/// in native runtime terms, while the raw callback-table and FFI details stay
/// encapsulated in `ffi.rs`.
pub struct ParentTransportAdapter<M> {
    marshaller: M,
}

impl<M> Clone for ParentTransportAdapter<M>
where
    M: Clone,
{
    fn clone(&self) -> Self {
        Self {
            marshaller: self.marshaller.clone(),
        }
    }
}

impl<M> ParentTransportAdapter<M>
where
    M: IMarshaller,
{
    pub fn new(marshaller: M) -> Self {
        Self { marshaller }
    }

    pub fn send(&self, params: SendParams) -> SendResult {
        self.marshaller.send(params)
    }

    pub fn post(&self, params: PostParams) {
        self.marshaller.post(params);
    }

    pub fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.marshaller.try_cast(params)
    }

    pub fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.marshaller.add_ref(params)
    }

    pub fn release(&self, params: ReleaseParams) -> StandardResult {
        self.marshaller.release(params)
    }

    pub fn object_released(&self, params: ObjectReleasedParams) {
        self.marshaller.object_released(params);
    }

    pub fn transport_down(&self, params: TransportDownParams) {
        self.marshaller.transport_down(params);
    }

    pub fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        self.marshaller.get_new_zone_id(params)
    }
}

impl ParentTransportAdapter<ParentCallbacks> {
    pub fn from_dll_init_params(params: &CanopyDllInitParams) -> Self {
        Self::new(ParentCallbacks::from_init_params(params))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::{
        CanopyBackChannelEntry, CanopyBackChannelSpan, CanopyConstByteBuffer, CanopyDllInitParams,
        borrow_remote_object, borrow_zone,
    };
    use crate::test_support::sample_zone;
    use canopy_rpc::internal::error_codes;
    use canopy_rpc::{
        AddRefParams, Encoding, GetNewZoneIdParams, Object, ObjectReleasedParams, PostParams,
        ReleaseParams, SendParams, TransportDownParams, TryCastParams, Zone,
    };
    use std::sync::Mutex;

    #[derive(Default)]
    struct RecordingMarshaller {
        last_send: Mutex<Option<SendParams>>,
    }

    impl IMarshaller for RecordingMarshaller {
        fn send(&self, params: SendParams) -> SendResult {
            *self
                .last_send
                .lock()
                .expect("last_send mutex should not be poisoned") = Some(params);
            SendResult::new(123, vec![9, 8, 7], Vec::new())
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

        fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
            NewZoneIdResult::new(error_codes::OK(), Zone::default(), Vec::new())
        }
    }

    #[test]
    fn child_transport_adapter_decodes_send_params() {
        let zone = sample_zone();
        let remote = zone
            .with_object(Object::new(17))
            .expect("with_object should succeed");
        let marshaller = RecordingMarshaller::default();
        let adapter = ChildTransportAdapter::new(marshaller);

        let back_channel_payload = [4u8, 5u8];
        let back_channel_entries = [CanopyBackChannelEntry {
            type_id: 66,
            payload: CanopyConstByteBuffer {
                data: back_channel_payload.as_ptr(),
                size: back_channel_payload.len(),
            },
        }];
        let input_data = [1u8, 2u8, 3u8];
        let raw = CanopySendParams {
            protocol_version: 3,
            encoding_type: Encoding::ProtocolBuffers as u64,
            tag: 19,
            caller_zone_id: borrow_zone(&zone),
            remote_object_id: borrow_remote_object(&remote),
            interface_id: 10,
            method_id: 11,
            in_data: CanopyConstByteBuffer {
                data: input_data.as_ptr(),
                size: input_data.len(),
            },
            in_back_channel: CanopyBackChannelSpan {
                data: back_channel_entries.as_ptr(),
                size: back_channel_entries.len(),
            },
        };

        let result = adapter.send(&raw);
        assert_eq!(result.error_code, 123);
        assert_eq!(result.out_buf, vec![9, 8, 7]);

        let captured = adapter
            .marshaller
            .last_send
            .lock()
            .expect("last_send mutex should not be poisoned")
            .clone()
            .expect("send should have been captured");
        assert_eq!(captured.protocol_version, 3);
        assert_eq!(captured.encoding_type, Encoding::ProtocolBuffers);
        assert_eq!(captured.tag, 19);
        assert_eq!(captured.interface_id.get_val(), 10);
        assert_eq!(captured.method_id.get_val(), 11);
        assert_eq!(captured.in_data, vec![1, 2, 3]);
        assert_eq!(captured.in_back_channel.len(), 1);
        assert_eq!(captured.in_back_channel[0].type_id, 66);
        assert_eq!(captured.in_back_channel[0].payload, vec![4, 5]);
    }

    #[test]
    fn parent_transport_adapter_forwards_send_to_marshaller() {
        let zone = sample_zone();
        let remote = zone
            .with_object(Object::new(23))
            .expect("with_object should succeed");
        let marshaller = RecordingMarshaller::default();
        let adapter = ParentTransportAdapter::new(marshaller);

        let result = adapter.send(SendParams {
            protocol_version: 3,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 55,
            caller_zone_id: zone,
            remote_object_id: remote,
            interface_id: canopy_rpc::InterfaceOrdinal::new(31),
            method_id: canopy_rpc::Method::new(32),
            in_data: vec![8, 7, 6],
            in_back_channel: Vec::new(),
        });

        assert_eq!(result.error_code, 123);
        assert_eq!(result.out_buf, vec![9, 8, 7]);
    }

    #[test]
    fn parent_transport_adapter_can_be_built_from_dll_init_params() {
        let init_params = CanopyDllInitParams::default();
        let adapter = ParentTransportAdapter::from_dll_init_params(&init_params);

        let result = adapter.send(SendParams::default());
        assert_eq!(result.error_code, error_codes::ZONE_NOT_FOUND());
    }
}
