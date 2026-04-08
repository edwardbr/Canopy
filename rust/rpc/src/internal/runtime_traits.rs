//! Hidden runtime trait-object seams for services and transports.
//!
//! These traits are the Rust counterpart to the C++ abstract entity
//! boundaries. They are intentionally kept internal so application code still
//! sees generated interfaces and pointer types rather than service/transport
//! plumbing.
//!
//! `ServiceRuntime` and `TransportRuntime` both extend `IMarshaller`, which is
//! the explicit Rust I/O boundary contract. No runtime lock may be held when
//! calling any of those marshaller methods.

use std::any::Any;
use std::sync::{Arc, Mutex, Weak};

use crate::internal::bindings_fwd::InterfacePointerKind;
use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{
    AddRefParams, PostParams, ReleaseParams, SendParams, SendResult, StandardResult, TryCastParams,
};
use crate::internal::pass_through::PassThrough;
use crate::internal::service_proxy::ProxyCaller;
use crate::internal::stub::ObjectStub;
use crate::rpc_types::{Encoding, InterfaceOrdinal, Object, RemoteObject, Zone};

pub trait ServiceRuntime: IMarshaller + Send + Sync {
    fn name(&self) -> &str;
    fn zone_id(&self) -> Zone;
    fn default_encoding(&self) -> Encoding;
    fn generate_new_object_id(&self) -> Object;
    fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>);
    fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>>;
    fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32;
    fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32>;
    fn lookup_local_interface_view_erased(
        &self,
        object_id: Object,
        interface_id: InterfaceOrdinal,
    ) -> Result<Arc<dyn Any + Send + Sync>, i32>;
    fn transport_down_from_params(&self, params: crate::TransportDownParams);
    fn concrete_service(&self) -> Option<&crate::internal::service::Service> {
        None
    }
    fn make_remote_caller(
        &self,
        remote_object_id: RemoteObject,
        pointer_kind: InterfacePointerKind,
    ) -> Option<Arc<dyn ProxyCaller>> {
        self.concrete_service()
            .and_then(|service| service.make_remote_caller(remote_object_id, pointer_kind))
    }

    fn outbound_send_via_transport(
        &self,
        params: SendParams,
        transport: Arc<dyn TransportRuntime>,
    ) -> SendResult {
        transport.send(params)
    }

    fn outbound_post_via_transport(
        &self,
        params: PostParams,
        transport: Arc<dyn TransportRuntime>,
    ) {
        transport.post(params);
    }

    fn outbound_try_cast_via_transport(
        &self,
        params: TryCastParams,
        transport: Arc<dyn TransportRuntime>,
    ) -> StandardResult {
        transport.try_cast(params)
    }

    fn outbound_add_ref_via_transport(
        &self,
        params: AddRefParams,
        transport: Arc<dyn TransportRuntime>,
    ) -> StandardResult {
        transport.add_ref(params)
    }

    fn outbound_release_via_transport(
        &self,
        params: ReleaseParams,
        transport: Arc<dyn TransportRuntime>,
    ) -> StandardResult {
        transport.release(params)
    }
}

pub trait TransportRuntime: IMarshaller + Send + Sync {
    fn name(&self) -> &str;
    fn zone_id(&self) -> Zone;
    fn adjacent_zone_id(&self) -> Zone;
    fn service(&self) -> Option<Arc<dyn ServiceRuntime>>;
    fn set_service(&self, service: Weak<dyn ServiceRuntime>);
    fn status(&self) -> crate::internal::transport::TransportStatus;
    fn set_status(&self, status: crate::internal::transport::TransportStatus);
    fn destination_count(&self) -> i64;
    fn increment_outbound_proxy_count(&self, dest: Zone);
    fn decrement_outbound_proxy_count(&self, dest: Zone) -> bool;
    fn increment_inbound_stub_count(&self, caller: Zone);
    fn decrement_inbound_stub_count(&self, caller: Zone) -> bool;
    fn decrement_inbound_stub_count_by(&self, caller: Zone, decrement: u64) -> bool;
    fn add_passthrough(&self, pass_through: &Arc<PassThrough>);
    fn get_passthrough(&self, zone1: Zone, zone2: Zone) -> Option<Arc<PassThrough>>;
    fn pass_through_entry_count_for_test(&self) -> usize;
    fn get_transport_from_passthroughs(
        &self,
        destination: &Zone,
    ) -> Option<Arc<dyn TransportRuntime>>;
    fn inbound_add_ref(self: Arc<Self>, params: crate::AddRefParams) -> crate::StandardResult;
    fn inbound_release(self: Arc<Self>, params: crate::ReleaseParams) -> crate::StandardResult;
}
