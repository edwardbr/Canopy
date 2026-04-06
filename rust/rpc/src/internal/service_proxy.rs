//! Rust counterpart of `c++/rpc/include/rpc/internal/service_proxy.h`.
//!
//! Despite the legacy filename, this module is the generated caller seam for
//! Rust proxies. It owns RPC call-context construction over the handwritten
//! runtime and should not imply any concrete transport implementation.

use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{SendParams, SendResult};
use crate::rpc_types::{
    BackChannelEntry, CallerZone, Encoding, InterfaceOrdinal, Method, RemoteObject,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedRpcCallContext {
    pub protocol_version: u64,
    pub encoding_type: Encoding,
    pub tag: u64,
    pub caller_zone_id: CallerZone,
    pub remote_object_id: RemoteObject,
}

pub trait GeneratedRpcCaller {
    fn marshaller(&self) -> &dyn IMarshaller;

    fn call_context(&self) -> GeneratedRpcCallContext;

    fn send_generated(
        &self,
        interface_id: InterfaceOrdinal,
        method_id: Method,
        in_data: Vec<u8>,
        in_back_channel: Vec<BackChannelEntry>,
    ) -> SendResult {
        let context = self.call_context();
        self.marshaller().send(SendParams {
            protocol_version: context.protocol_version,
            encoding_type: context.encoding_type,
            tag: context.tag,
            caller_zone_id: context.caller_zone_id,
            remote_object_id: context.remote_object_id,
            interface_id,
            method_id,
            in_data,
            in_back_channel,
        })
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use super::{GeneratedRpcCallContext, GeneratedRpcCaller};
    use crate::internal::{
        AddRefParams, GetNewZoneIdParams, IMarshaller, NewZoneIdResult, ObjectReleasedParams,
        PostParams, ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams,
        TryCastParams,
    };
    use crate::rpc_types::{
        CallerZone, Encoding, InterfaceOrdinal, Method, RemoteObject, ZoneAddress, ZoneAddressArgs,
    };

    #[derive(Default)]
    struct FakeMarshaller {
        last_send: Mutex<Option<SendParams>>,
    }

    impl IMarshaller for FakeMarshaller {
        fn send(&self, params: SendParams) -> SendResult {
            *self.last_send.lock().expect("last_send mutex poisoned") = Some(params);
            SendResult::new(0, vec![1, 2, 3], vec![])
        }

        fn post(&self, _params: PostParams) {}

        fn try_cast(&self, _params: TryCastParams) -> StandardResult {
            StandardResult::default()
        }

        fn add_ref(&self, _params: AddRefParams) -> StandardResult {
            StandardResult::default()
        }

        fn release(&self, _params: ReleaseParams) -> StandardResult {
            StandardResult::default()
        }

        fn object_released(&self, _params: ObjectReleasedParams) {}

        fn transport_down(&self, _params: TransportDownParams) {}

        fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
            NewZoneIdResult::default()
        }
    }

    struct FakeCaller {
        marshaller: Arc<FakeMarshaller>,
        context: GeneratedRpcCallContext,
    }

    impl GeneratedRpcCaller for FakeCaller {
        fn marshaller(&self) -> &dyn IMarshaller {
            self.marshaller.as_ref()
        }

        fn call_context(&self) -> GeneratedRpcCallContext {
            self.context.clone()
        }
    }

    #[test]
    fn generated_rpc_caller_builds_send_params_from_context() {
        let zone = ZoneAddress::create(ZoneAddressArgs::default())
            .expect("default zone address should be valid");
        let marshaller = Arc::new(FakeMarshaller::default());
        let caller = FakeCaller {
            marshaller: marshaller.clone(),
            context: GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 77,
                caller_zone_id: CallerZone::from(zone.clone()),
                remote_object_id: RemoteObject::from(zone),
            },
        };

        let result = caller.send_generated(
            InterfaceOrdinal::new(42),
            Method::new(7),
            vec![9, 8],
            vec![],
        );

        assert_eq!(result.out_buf, vec![1, 2, 3]);

        let sent = marshaller
            .last_send
            .lock()
            .expect("last_send mutex poisoned")
            .clone()
            .expect("send should have been captured");
        assert_eq!(sent.protocol_version, 3);
        assert_eq!(sent.encoding_type, Encoding::ProtocolBuffers);
        assert_eq!(sent.tag, 77);
        assert_eq!(sent.interface_id, InterfaceOrdinal::new(42));
        assert_eq!(sent.method_id, Method::new(7));
        assert_eq!(sent.in_data, vec![9, 8]);
    }
}
