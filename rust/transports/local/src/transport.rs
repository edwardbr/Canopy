//! Rust counterpart of `c++/transports/local/include/transports/local/transport.h`.
//!
//! The primary entities mirror the C++ local transport family:
//! - `ChildTransport`: parent zone -> child zone
//! - `ParentTransport`: child zone -> parent zone
//! - `ChildZone`: helper that binds a child service to that transport pair

use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddRefParams, GetNewZoneIdParams, IMarshaller, NewZoneIdResult, Object, ObjectReleasedParams,
    PostParams, ReleaseParams, RemoteObject, SendParams, SendResult, ServiceRuntime,
    StandardResult, Transport, TransportDownParams, TransportStatus, TryCastParams,
};
use std::sync::{Arc, Mutex, Weak};

#[derive(Default)]
struct LocalPeerMarshaller {
    peer: Mutex<Weak<Transport>>,
}

impl LocalPeerMarshaller {
    fn set_peer(&self, peer: &Arc<Transport>) {
        *self.peer.lock().expect("local peer mutex poisoned") = Arc::downgrade(peer);
    }

    fn peer(&self) -> Option<Arc<Transport>> {
        self.peer
            .lock()
            .expect("local peer mutex poisoned")
            .upgrade()
    }
}

fn weak_service(service: &Arc<dyn ServiceRuntime>) -> Weak<dyn ServiceRuntime> {
    Arc::downgrade(service)
}

impl IMarshaller for LocalPeerMarshaller {
    fn send(&self, params: SendParams) -> SendResult {
        let Some(peer) = self.peer() else {
            return SendResult::new(error_codes::ZONE_NOT_FOUND(), vec![], vec![]);
        };
        peer.service()
            .map(|service| service.send(params))
            .unwrap_or_else(|| SendResult::new(error_codes::ZONE_NOT_FOUND(), vec![], vec![]))
    }

    fn post(&self, params: PostParams) {
        if let Some(service) = self.peer().and_then(|peer| peer.service()) {
            service.post(params);
        }
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.peer()
            .and_then(|peer| peer.service())
            .map(|service| service.try_cast(params))
            .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]))
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        let Some(peer) = self.peer() else {
            return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
        };
        peer.inbound_add_ref(params)
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        self.peer()
            .map(|peer| peer.inbound_release(params))
            .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]))
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        if let Some(service) = self.peer().and_then(|peer| peer.service()) {
            service.object_released(params);
        }
    }

    fn transport_down(&self, params: TransportDownParams) {
        if let Some(service) = self.peer().and_then(|peer| peer.service()) {
            service.transport_down_from_params(params);
        }
    }

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        self.peer()
            .and_then(|peer| peer.service())
            .map(|service| service.get_new_zone_id(params))
            .unwrap_or_else(|| {
                NewZoneIdResult::new(error_codes::ZONE_NOT_FOUND(), Default::default(), vec![])
            })
    }
}

#[derive(Clone)]
pub struct ChildTransport {
    transport: Arc<Transport>,
}

impl ChildTransport {
    pub fn transport(&self) -> Arc<Transport> {
        self.transport.clone()
    }
}

#[derive(Clone)]
pub struct ParentTransport {
    transport: Arc<Transport>,
}

impl ParentTransport {
    pub fn transport(&self) -> Arc<Transport> {
        self.transport.clone()
    }
}

#[derive(Clone)]
pub struct TransportPair {
    child_transport: ChildTransport,
    parent_transport: ParentTransport,
}

#[derive(Clone)]
pub struct ChildZone {
    service: Arc<canopy_rpc::ChildService>,
    transports: TransportPair,
}

#[derive(Clone)]
pub struct BoundChildZone {
    zone: ChildZone,
    root_object: RemoteObject,
}

impl ChildZone {
    pub fn create(
        name: impl Into<String>,
        parent_service: Arc<dyn ServiceRuntime>,
        child_name: impl Into<String>,
        child_zone_id: canopy_rpc::Zone,
    ) -> Self {
        let service = canopy_rpc::ChildService::new_shared(
            child_name,
            child_zone_id,
            parent_service.zone_id(),
        );
        let transports = TransportPair::connect(name, parent_service, service.clone());
        service.set_parent_transport(transports.parent_transport().transport());
        Self {
            service,
            transports,
        }
    }

    pub fn service(&self) -> &Arc<canopy_rpc::ChildService> {
        &self.service
    }

    pub fn transports(&self) -> &TransportPair {
        &self.transports
    }

    pub fn child_transport(&self) -> &ChildTransport {
        self.transports.child_transport()
    }

    pub fn parent_transport(&self) -> &ParentTransport {
        self.transports.parent_transport()
    }

    pub fn bind_root_rpc_object(
        self,
        parent_service: Arc<dyn ServiceRuntime>,
        object_id: Object,
        root_object: Arc<dyn canopy_rpc::RpcObject>,
        pointer_kind: canopy_rpc::InterfacePointerKind,
    ) -> Result<BoundChildZone, i32> {
        self.service()
            .register_rpc_object(object_id, root_object)
            .map_err(|error_code| error_code)?;
        let remote_object = self
            .service()
            .zone_id()
            .with_object(object_id)
            .map_err(|_| error_codes::INVALID_DATA())?;
        let add_ref = self.service().add_ref(AddRefParams {
            protocol_version: canopy_rpc::DefaultValues::VERSION_3 as u64,
            remote_object_id: remote_object.clone(),
            caller_zone_id: parent_service.zone_id(),
            requesting_zone_id: self.service().zone_id(),
            build_out_param_channel: canopy_rpc::add_ref_options_for_pointer_kind(pointer_kind),
            in_back_channel: vec![],
        });
        if error_codes::is_error(add_ref.error_code) {
            return Err(add_ref.error_code);
        }
        Ok(BoundChildZone {
            zone: self,
            root_object: remote_object,
        })
    }
}

impl BoundChildZone {
    pub fn zone(&self) -> &ChildZone {
        &self.zone
    }

    pub fn root_object(&self) -> &RemoteObject {
        &self.root_object
    }
}

impl TransportPair {
    pub fn connect(
        name: impl Into<String>,
        parent_service: Arc<dyn ServiceRuntime>,
        child_service: Arc<dyn ServiceRuntime>,
    ) -> Self {
        let name = name.into();
        let parent_to_child_peer = Arc::new(LocalPeerMarshaller::default());
        let child_to_parent_peer = Arc::new(LocalPeerMarshaller::default());

        let parent_to_child = Transport::new(
            format!("{name}:parent-to-child"),
            parent_service.zone_id(),
            child_service.zone_id(),
            weak_service(&parent_service),
            parent_to_child_peer.clone(),
        );
        let child_to_parent = Transport::new(
            format!("{name}:child-to-parent"),
            child_service.zone_id(),
            parent_service.zone_id(),
            weak_service(&child_service),
            child_to_parent_peer.clone(),
        );

        parent_to_child_peer.set_peer(&child_to_parent);
        child_to_parent_peer.set_peer(&parent_to_child);
        parent_to_child.set_status(TransportStatus::Connected);
        child_to_parent.set_status(TransportStatus::Connected);
        parent_service.add_transport(child_service.zone_id(), parent_to_child.clone());
        child_service.add_transport(parent_service.zone_id(), child_to_parent.clone());

        Self {
            child_transport: ChildTransport {
                transport: parent_to_child,
            },
            parent_transport: ParentTransport {
                transport: child_to_parent,
            },
        }
    }

    pub fn child_transport(&self) -> &ChildTransport {
        &self.child_transport
    }

    pub fn parent_transport(&self) -> &ParentTransport {
        &self.parent_transport
    }
}

#[doc(hidden)]
pub type LocalChildTransport = ChildTransport;
#[doc(hidden)]
pub type LocalParentTransport = ParentTransport;
#[doc(hidden)]
pub type LocalTransportPair = TransportPair;
#[doc(hidden)]
pub type LocalChildZone = ChildZone;
#[doc(hidden)]
pub type LocalBoundChildZone = BoundChildZone;

#[cfg(test)]
mod tests {
    use super::*;
    use canopy_rpc::{
        AddressType, ChildService, DefaultValues, Encoding, InterfaceOrdinal, Method, Object,
        RemoteObject, RootService, Zone, ZoneAddress, ZoneAddressArgs,
    };
    use std::sync::Mutex;

    #[derive(Debug)]
    struct RecordingTarget {
        calls: Mutex<u32>,
    }

    impl canopy_rpc::CastingInterface for RecordingTarget {
        fn __rpc_query_interface(&self, _interface_id: InterfaceOrdinal) -> bool {
            true
        }

        fn __rpc_call(&self, _params: SendParams) -> SendResult {
            *self.calls.lock().expect("calls mutex poisoned") += 1;
            SendResult::new(error_codes::OK(), vec![1, 2, 3], vec![])
        }
    }

    impl canopy_rpc::GeneratedRustInterface for RecordingTarget {
        fn interface_name() -> &'static str {
            "RecordingTarget"
        }

        fn get_id(_rpc_version: u64) -> u64 {
            77
        }

        fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
            &[]
        }
    }

    fn zone(subnet: u32) -> Zone {
        Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Local,
                0,
                vec![],
                DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
                subnet.into(),
                DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
                0,
                vec![],
            ))
            .expect("zone address should be valid"),
        )
    }

    #[test]
    fn local_pair_forwards_parent_to_child_send() {
        let parent = RootService::new_shared("parent", zone(1));
        let child = ChildService::new_shared("child", zone(2), parent.zone_id());
        let _pair = TransportPair::connect("test", parent.clone(), child.clone());
        let target = Arc::new(RecordingTarget {
            calls: Mutex::new(0),
        });
        child
            .register_local_object(Object::new(9), target.clone())
            .expect("register child target");
        let remote = child.zone_id().with_object(Object::new(9)).expect("remote");
        let result = parent
            .get_transport(&child.zone_id())
            .expect("parent transport")
            .send(SendParams {
                protocol_version: DefaultValues::VERSION_3 as u64,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 0,
                caller_zone_id: parent.zone_id(),
                remote_object_id: remote,
                interface_id: InterfaceOrdinal::new(77),
                method_id: Method::new(1),
                in_data: vec![],
                in_back_channel: vec![],
            });
        assert_eq!(result.error_code, error_codes::OK());
        assert_eq!(*target.calls.lock().expect("calls mutex poisoned"), 1);
    }

    #[test]
    fn local_pair_forwards_child_to_parent_add_ref() {
        let parent = RootService::new_shared("parent", zone(10));
        let child = ChildService::new_shared("child", zone(11), parent.zone_id());
        let _pair = TransportPair::connect("test", parent.clone(), child.clone());
        let target = Arc::new(RecordingTarget {
            calls: Mutex::new(0),
        });
        parent
            .register_local_object(Object::new(3), target)
            .expect("register parent target");
        let remote = RemoteObject::new(parent.zone_id().get_address().clone())
            .with_object(Object::new(3))
            .expect("remote");
        let result = child
            .get_transport(&parent.zone_id())
            .expect("child transport")
            .add_ref(AddRefParams {
                protocol_version: DefaultValues::VERSION_3 as u64,
                remote_object_id: remote,
                caller_zone_id: child.zone_id(),
                requesting_zone_id: child.zone_id(),
                build_out_param_channel: canopy_rpc::AddRefOptions::NORMAL,
                in_back_channel: vec![],
            });
        assert_eq!(result.error_code, error_codes::OK());
    }
}
