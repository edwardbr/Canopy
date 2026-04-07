//! Rust counterpart of `c++/rpc/include/rpc/internal/service_proxy.h`.

use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::Weak;

use crate::internal::bindings_fwd::InterfacePointerKind;
use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{
    AddRefParams, ReleaseParams, SendParams, SendResult, StandardResult,
};
use crate::internal::object_proxy::{ObjectProxy, RemoteRefGuard};
use crate::internal::service::Service;
use crate::internal::transport::Transport;
use crate::rpc_types::{
    AddRefOptions, BackChannelEntry, CallerZone, Encoding, InterfaceOrdinal, Method, Object,
    ReleaseOptions, RemoteObject, RequestingZone,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedRpcCallContext {
    pub protocol_version: u64,
    pub encoding_type: Encoding,
    pub tag: u64,
    pub caller_zone_id: CallerZone,
    pub remote_object_id: RemoteObject,
}

pub trait GeneratedRpcCaller: Send + Sync {
    fn marshaller(&self) -> &dyn IMarshaller;

    fn call_context(&self) -> GeneratedRpcCallContext;

    fn object_proxy(&self) -> Option<Arc<ObjectProxy>> {
        None
    }

    fn local_service(&self) -> Option<&Service> {
        None
    }

    fn make_remote_caller(
        &self,
        _remote_object_id: RemoteObject,
    ) -> Option<Arc<dyn GeneratedRpcCaller>> {
        None
    }

    fn make_remote_caller_with_ref(
        &self,
        remote_object_id: RemoteObject,
        _pointer_kind: InterfacePointerKind,
    ) -> Option<Arc<dyn GeneratedRpcCaller>> {
        self.make_remote_caller(remote_object_id)
    }

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

    fn add_ref_generated(
        &self,
        remote_object_id: RemoteObject,
        build_out_param_channel: AddRefOptions,
        requesting_zone_id: RequestingZone,
    ) -> StandardResult {
        let context = self.call_context();
        self.marshaller().add_ref(AddRefParams {
            protocol_version: context.protocol_version,
            remote_object_id,
            caller_zone_id: context.caller_zone_id,
            requesting_zone_id,
            build_out_param_channel,
            in_back_channel: vec![],
        })
    }

    fn release_generated(
        &self,
        remote_object_id: RemoteObject,
        options: ReleaseOptions,
    ) -> StandardResult {
        let context = self.call_context();
        self.marshaller().release(ReleaseParams {
            protocol_version: context.protocol_version,
            remote_object_id,
            caller_zone_id: context.caller_zone_id,
            options,
            in_back_channel: vec![],
        })
    }
}

pub struct ServiceProxy {
    service: Arc<Service>,
    transport: Option<Arc<Transport>>,
    context: GeneratedRpcCallContext,
    object_proxies: Arc<Mutex<BTreeMap<Object, Weak<ObjectProxy>>>>,
}

impl std::fmt::Debug for ServiceProxy {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ServiceProxy")
            .field("service_zone", &self.service.zone_id())
            .field("has_transport", &self.transport.is_some())
            .field("context", &self.context)
            .field(
                "object_proxy_count",
                &self
                    .object_proxies
                    .lock()
                    .expect("service proxy object_proxies mutex poisoned")
                    .len(),
            )
            .finish_non_exhaustive()
    }
}

impl ServiceProxy {
    pub fn local(service: Arc<Service>, context: GeneratedRpcCallContext) -> Arc<Self> {
        Arc::new(Self {
            service,
            transport: None,
            context,
            object_proxies: Arc::new(Mutex::new(BTreeMap::new())),
        })
    }

    pub fn with_transport(
        service: Arc<Service>,
        transport: Arc<Transport>,
        context: GeneratedRpcCallContext,
    ) -> Arc<Self> {
        Arc::new(Self {
            service,
            transport: Some(transport),
            context,
            object_proxies: Arc::new(Mutex::new(BTreeMap::new())),
        })
    }

    pub fn service(&self) -> &Arc<Service> {
        &self.service
    }

    pub fn transport(&self) -> Option<Arc<Transport>> {
        self.transport.clone()
    }

    pub fn object_proxy_entry_count_for_test(&self) -> usize {
        self.object_proxies
            .lock()
            .expect("service proxy object_proxies mutex poisoned")
            .len()
    }

    fn prune_dead_object_proxies(proxies: &mut BTreeMap<Object, Weak<ObjectProxy>>) {
        proxies.retain(|_, proxy| proxy.strong_count() > 0);
    }

    pub fn get_or_create_object_proxy(
        self: &Arc<Self>,
        remote_object_id: RemoteObject,
    ) -> Arc<ObjectProxy> {
        let object_id = remote_object_id.get_object_id();
        let mut proxies = self
            .object_proxies
            .lock()
            .expect("service proxy object_proxies mutex poisoned");
        Self::prune_dead_object_proxies(&mut proxies);

        if let Some(existing) = proxies.get(&object_id).and_then(Weak::upgrade) {
            return existing;
        }

        let proxy = Arc::new(ObjectProxy::new_remote(remote_object_id, self));
        proxies.insert(object_id, Arc::downgrade(&proxy));
        proxy
    }

    pub fn proxy_for_remote_object(
        self: &Arc<Self>,
        remote_object_id: RemoteObject,
    ) -> Arc<ObjectProxyCaller> {
        let object_proxy = self.get_or_create_object_proxy(remote_object_id.clone());
        Arc::new(ObjectProxyCaller {
            object_proxy,
            context: GeneratedRpcCallContext {
                remote_object_id,
                ..self.context.clone()
            },
            remote_ref: None,
        })
    }

    pub fn proxy_for_remote_object_with_ref(
        self: &Arc<Self>,
        remote_object_id: RemoteObject,
        pointer_kind: InterfacePointerKind,
    ) -> Arc<ObjectProxyCaller> {
        let object_proxy = self.get_or_create_object_proxy(remote_object_id.clone());
        let remote_ref =
            RemoteRefGuard::adopt(object_proxy.clone(), self.service.zone_id(), pointer_kind);
        Arc::new(ObjectProxyCaller {
            object_proxy,
            context: GeneratedRpcCallContext {
                remote_object_id,
                ..self.context.clone()
            },
            remote_ref,
        })
    }

    pub fn add_ref_remote_object_for_caller(
        &self,
        object_id: Object,
        caller_zone_id: CallerZone,
        pointer_kind: InterfacePointerKind,
    ) -> StandardResult {
        let Some(transport) = &self.transport else {
            return StandardResult::new(crate::TRANSPORT_ERROR(), vec![]);
        };
        let Some(remote_object_id) = self
            .context
            .remote_object_id
            .as_zone()
            .with_object(object_id)
            .ok()
        else {
            return StandardResult::new(crate::INVALID_DATA(), vec![]);
        };
        let pointer_options = if pointer_kind.is_optimistic() {
            AddRefOptions::OPTIMISTIC
        } else {
            AddRefOptions::NORMAL
        };
        transport.add_ref(AddRefParams {
            protocol_version: self.context.protocol_version,
            remote_object_id,
            caller_zone_id,
            requesting_zone_id: self.service.zone_id(),
            build_out_param_channel: AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | pointer_options,
            in_back_channel: vec![],
        })
    }

    pub fn release_remote_object_for_caller(
        &self,
        object_id: Object,
        caller_zone_id: CallerZone,
        options: ReleaseOptions,
    ) -> StandardResult {
        let Some(transport) = &self.transport else {
            return StandardResult::new(crate::TRANSPORT_ERROR(), vec![]);
        };
        let Some(remote_object_id) = self
            .context
            .remote_object_id
            .as_zone()
            .with_object(object_id)
            .ok()
        else {
            return StandardResult::new(crate::INVALID_DATA(), vec![]);
        };
        transport.release(ReleaseParams {
            protocol_version: self.context.protocol_version,
            remote_object_id,
            caller_zone_id,
            options,
            in_back_channel: vec![],
        })
    }
}

pub struct ObjectProxyCaller {
    object_proxy: Arc<ObjectProxy>,
    context: GeneratedRpcCallContext,
    remote_ref: Option<RemoteRefGuard>,
}

impl ObjectProxyCaller {
    pub fn object_proxy(&self) -> Arc<ObjectProxy> {
        self.object_proxy.clone()
    }
}

impl std::fmt::Debug for ObjectProxyCaller {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ObjectProxyCaller")
            .field("object_proxy", &self.object_proxy)
            .field("context", &self.context)
            .field("remote_ref", &self.remote_ref)
            .finish()
    }
}

impl GeneratedRpcCaller for ObjectProxyCaller {
    fn marshaller(&self) -> &dyn IMarshaller {
        self.object_proxy
            .service_proxy_ref()
            .expect("remote object proxy should keep its service proxy alive")
            .marshaller()
    }

    fn call_context(&self) -> GeneratedRpcCallContext {
        self.context.clone()
    }

    fn object_proxy(&self) -> Option<Arc<ObjectProxy>> {
        Some(self.object_proxy.clone())
    }

    fn local_service(&self) -> Option<&Service> {
        self.object_proxy
            .service_proxy_ref()
            .map(|service_proxy| service_proxy.service.as_ref())
    }

    fn make_remote_caller(
        &self,
        remote_object_id: RemoteObject,
    ) -> Option<Arc<dyn GeneratedRpcCaller>> {
        let service_proxy = self.object_proxy.service_proxy()?;
        Some(service_proxy.proxy_for_remote_object(remote_object_id))
    }

    fn make_remote_caller_with_ref(
        &self,
        remote_object_id: RemoteObject,
        pointer_kind: InterfacePointerKind,
    ) -> Option<Arc<dyn GeneratedRpcCaller>> {
        let service_proxy = self.object_proxy.service_proxy()?;
        Some(service_proxy.proxy_for_remote_object_with_ref(remote_object_id, pointer_kind))
    }
}

impl GeneratedRpcCaller for ServiceProxy {
    fn marshaller(&self) -> &dyn IMarshaller {
        match &self.transport {
            Some(transport) => transport.as_ref(),
            None => self.service.as_ref(),
        }
    }

    fn call_context(&self) -> GeneratedRpcCallContext {
        self.context.clone()
    }

    fn local_service(&self) -> Option<&Service> {
        Some(self.service.as_ref())
    }

    fn make_remote_caller(
        &self,
        remote_object_id: RemoteObject,
    ) -> Option<Arc<dyn GeneratedRpcCaller>> {
        let service_proxy = Arc::new(Self {
            service: self.service.clone(),
            transport: self.transport.clone(),
            context: self.context.clone(),
            object_proxies: self.object_proxies.clone(),
        });
        Some(service_proxy.proxy_for_remote_object(remote_object_id))
    }

    fn make_remote_caller_with_ref(
        &self,
        remote_object_id: RemoteObject,
        pointer_kind: InterfacePointerKind,
    ) -> Option<Arc<dyn GeneratedRpcCaller>> {
        let service_proxy = Arc::new(Self {
            service: self.service.clone(),
            transport: self.transport.clone(),
            context: self.context.clone(),
            object_proxies: self.object_proxies.clone(),
        });
        Some(service_proxy.proxy_for_remote_object_with_ref(remote_object_id, pointer_kind))
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use super::{GeneratedRpcCallContext, GeneratedRpcCaller, ServiceProxy};
    use crate::internal::{
        AddRefParams, GetNewZoneIdParams, IMarshaller, NewZoneIdResult, ObjectReleasedParams,
        PostParams, ReleaseParams, SendParams, SendResult, Service, StandardResult, Transport,
        TransportDownParams, TryCastParams,
    };
    use crate::rpc_types::{
        AddRefOptions, AddressType, CallerZone, DefaultValues, Encoding, InterfaceOrdinal, Method,
        Object, ReleaseOptions, RemoteObject, Zone, ZoneAddress, ZoneAddressArgs,
    };

    #[derive(Default)]
    struct FakeMarshaller {
        last_send: Mutex<Option<SendParams>>,
        last_add_ref: Mutex<Option<AddRefParams>>,
        add_refs: Mutex<Vec<AddRefParams>>,
        last_release: Mutex<Option<ReleaseParams>>,
        releases: Mutex<Vec<ReleaseParams>>,
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

        fn add_ref(&self, params: AddRefParams) -> StandardResult {
            let captured = params.clone();
            *self
                .last_add_ref
                .lock()
                .expect("last_add_ref mutex poisoned") = Some(params);
            self.add_refs
                .lock()
                .expect("add_refs mutex poisoned")
                .push(captured);
            StandardResult::default()
        }

        fn release(&self, params: ReleaseParams) -> StandardResult {
            let captured = params.clone();
            *self
                .last_release
                .lock()
                .expect("last_release mutex poisoned") = Some(params);
            self.releases
                .lock()
                .expect("releases mutex poisoned")
                .push(captured);
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

    fn zone(subnet: u64) -> Zone {
        Zone::from(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Local,
                0,
                vec![],
                DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
                subnet,
                DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
                0,
                vec![],
            ))
            .expect("zone address"),
        )
    }

    fn remote_object(subnet: u64, object: u64) -> RemoteObject {
        RemoteObject::from(
            zone(subnet)
                .with_object(Object::new(object))
                .expect("remote object"),
        )
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

    #[test]
    fn generated_rpc_caller_builds_add_ref_and_release_params_with_requesting_zone() {
        let zone = ZoneAddress::create(ZoneAddressArgs::default())
            .expect("default zone address should be valid");
        let remote_object_id = RemoteObject::from(
            crate::Zone::from(zone.clone())
                .with_object(Object::new(55))
                .unwrap(),
        );
        let marshaller = Arc::new(FakeMarshaller::default());
        let caller = FakeCaller {
            marshaller: marshaller.clone(),
            context: GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 77,
                caller_zone_id: CallerZone::from(zone.clone()),
                remote_object_id: remote_object_id.clone(),
            },
        };

        caller.add_ref_generated(
            remote_object_id.clone(),
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::OPTIMISTIC,
            crate::Zone::from(zone.clone()).into(),
        );
        caller.release_generated(remote_object_id.clone(), ReleaseOptions::OPTIMISTIC);

        let add_ref = marshaller
            .last_add_ref
            .lock()
            .expect("last_add_ref mutex poisoned")
            .clone()
            .expect("add_ref should be captured");
        assert_eq!(add_ref.protocol_version, 3);
        assert_eq!(add_ref.remote_object_id, remote_object_id);
        assert_eq!(add_ref.caller_zone_id, CallerZone::from(zone.clone()));
        assert_eq!(add_ref.requesting_zone_id, crate::Zone::from(zone.clone()));
        assert_eq!(
            add_ref.build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::OPTIMISTIC
        );

        let release = marshaller
            .last_release
            .lock()
            .expect("last_release mutex poisoned")
            .clone()
            .expect("release should be captured");
        assert_eq!(release.protocol_version, 3);
        assert_eq!(release.caller_zone_id, CallerZone::from(zone));
        assert_eq!(release.options, ReleaseOptions::OPTIMISTIC);
    }

    #[test]
    fn service_proxy_reuses_object_proxy_for_same_remote_object() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let remote = remote_object(2, 44);
        let service_proxy = ServiceProxy::local(
            service,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 1,
                caller_zone_id: CallerZone::from(zone(1)),
                remote_object_id: remote.clone(),
            },
        );

        let first = service_proxy.proxy_for_remote_object(remote.clone());
        let second = service_proxy.proxy_for_remote_object(remote.clone());
        let first_object_proxy = first.object_proxy();
        let second_object_proxy = second.object_proxy();

        assert!(Arc::ptr_eq(&first_object_proxy, &second_object_proxy));
        assert_eq!(first_object_proxy.remote_object_id(), Some(remote));
        assert!(first_object_proxy.service_proxy().is_some());
    }

    #[test]
    fn service_proxy_prunes_dead_object_proxy_entries_on_next_lookup() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let remote = remote_object(2, 45);
        let service_proxy = ServiceProxy::local(
            service,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 1,
                caller_zone_id: CallerZone::from(zone(1)),
                remote_object_id: remote.clone(),
            },
        );

        let proxy = service_proxy.proxy_for_remote_object(remote);
        assert_eq!(service_proxy.object_proxy_entry_count_for_test(), 1);
        drop(proxy);

        let _replacement = service_proxy.proxy_for_remote_object(remote_object(2, 46));
        assert_eq!(service_proxy.object_proxy_entry_count_for_test(), 1);
    }

    #[test]
    fn transport_backed_service_proxy_sends_through_transport() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(FakeMarshaller::default());
        let transport = Transport::new(
            "remote",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            marshaller.clone(),
        );
        let remote = remote_object(2, 55);
        let service_proxy = ServiceProxy::with_transport(
            service,
            transport,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 2,
                caller_zone_id: CallerZone::from(zone(1)),
                remote_object_id: remote.clone(),
            },
        );

        let result = service_proxy.send_generated(
            InterfaceOrdinal::new(9),
            Method::new(10),
            vec![1, 2],
            vec![],
        );

        assert_eq!(result.out_buf, vec![1, 2, 3]);
        let sent = marshaller
            .last_send
            .lock()
            .expect("last_send mutex poisoned")
            .clone()
            .expect("send should be captured");
        assert_eq!(sent.remote_object_id, remote);
        assert_eq!(sent.interface_id, InterfaceOrdinal::new(9));
        assert_eq!(sent.method_id, Method::new(10));
    }

    #[test]
    fn service_proxy_add_ref_preserves_y_topology_requesting_zone() {
        let zone5_service = Arc::new(Service::new("zone5", zone(5)));
        let marshaller = Arc::new(FakeMarshaller::default());
        let zone7_transport = Transport::new(
            "zone5-to-zone7",
            zone(5),
            zone(7),
            Arc::downgrade(&zone5_service),
            marshaller.clone(),
        );
        let zone7_object = remote_object(7, 77);
        let service_proxy = ServiceProxy::with_transport(
            zone5_service,
            zone7_transport,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 2,
                caller_zone_id: CallerZone::from(zone(5)),
                remote_object_id: zone7_object.clone(),
            },
        );
        let object_proxy = service_proxy
            .proxy_for_remote_object(zone7_object.clone())
            .object_proxy();

        let result = object_proxy.add_ref_remote_for_caller(
            CallerZone::from(zone(1)),
            crate::InterfacePointerKind::Shared,
        );

        assert_eq!(result.error_code, crate::OK());
        let add_ref = marshaller
            .last_add_ref
            .lock()
            .expect("last_add_ref mutex poisoned")
            .clone()
            .expect("add_ref should have been captured");
        assert_eq!(add_ref.protocol_version, 3);
        assert_eq!(add_ref.remote_object_id, zone7_object);
        assert_eq!(add_ref.caller_zone_id, CallerZone::from(zone(1)));
        assert_eq!(add_ref.requesting_zone_id, zone(5));
        assert_eq!(
            add_ref.build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::NORMAL
        );
    }

    #[test]
    fn service_proxy_optimistic_add_ref_preserves_y_topology_requesting_zone() {
        let zone5_service = Arc::new(Service::new("zone5", zone(5)));
        let marshaller = Arc::new(FakeMarshaller::default());
        let zone7_transport = Transport::new(
            "zone5-to-zone7",
            zone(5),
            zone(7),
            Arc::downgrade(&zone5_service),
            marshaller.clone(),
        );
        let zone7_object = remote_object(7, 78);
        let service_proxy = ServiceProxy::with_transport(
            zone5_service,
            zone7_transport,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 2,
                caller_zone_id: CallerZone::from(zone(5)),
                remote_object_id: zone7_object.clone(),
            },
        );
        let object_proxy = service_proxy
            .proxy_for_remote_object(zone7_object.clone())
            .object_proxy();

        let result = object_proxy.add_ref_remote_for_caller(
            CallerZone::from(zone(1)),
            crate::InterfacePointerKind::Optimistic,
        );

        assert_eq!(result.error_code, crate::OK());
        let add_ref = marshaller
            .last_add_ref
            .lock()
            .expect("last_add_ref mutex poisoned")
            .clone()
            .expect("add_ref should have been captured");
        assert_eq!(add_ref.protocol_version, 3);
        assert_eq!(add_ref.remote_object_id, zone7_object);
        assert_eq!(add_ref.caller_zone_id, CallerZone::from(zone(1)));
        assert_eq!(add_ref.requesting_zone_id, zone(5));
        assert_eq!(
            add_ref.build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::OPTIMISTIC
        );
    }

    #[test]
    fn object_proxy_sends_one_remote_add_ref_and_release_per_pointer_kind_transition() {
        let zone5_service = Arc::new(Service::new("zone5", zone(5)));
        let marshaller = Arc::new(FakeMarshaller::default());
        let zone7_transport = Transport::new(
            "zone5-to-zone7",
            zone(5),
            zone(7),
            Arc::downgrade(&zone5_service),
            marshaller.clone(),
        );
        let zone7_object = remote_object(7, 79);
        let service_proxy = ServiceProxy::with_transport(
            zone5_service,
            zone7_transport,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 2,
                caller_zone_id: CallerZone::from(zone(5)),
                remote_object_id: zone7_object.clone(),
            },
        );
        let object_proxy = service_proxy
            .proxy_for_remote_object(zone7_object.clone())
            .object_proxy();

        assert_eq!(
            object_proxy
                .add_ref_remote_for_caller(
                    CallerZone::from(zone(1)),
                    crate::InterfacePointerKind::Shared
                )
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .add_ref_remote_for_caller(
                    CallerZone::from(zone(1)),
                    crate::InterfacePointerKind::Shared
                )
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .add_ref_remote_for_caller(
                    CallerZone::from(zone(1)),
                    crate::InterfacePointerKind::Optimistic
                )
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .add_ref_remote_for_caller(
                    CallerZone::from(zone(1)),
                    crate::InterfacePointerKind::Optimistic
                )
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .add_ref_remote_for_caller(
                    CallerZone::from(zone(2)),
                    crate::InterfacePointerKind::Shared
                )
                .error_code,
            crate::OK()
        );

        let add_refs = marshaller
            .add_refs
            .lock()
            .expect("add_refs mutex poisoned")
            .clone();
        assert_eq!(add_refs.len(), 3);
        assert_eq!(
            add_refs[0].build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::NORMAL
        );
        assert_eq!(
            add_refs[1].build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::OPTIMISTIC
        );
        assert_eq!(
            add_refs[2].build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::NORMAL
        );
        assert_eq!(add_refs[2].caller_zone_id, CallerZone::from(zone(2)));
        assert_eq!(object_proxy.shared_count(), 3);
        assert_eq!(object_proxy.optimistic_count(), 2);

        assert_eq!(
            object_proxy
                .release_remote_for_caller(CallerZone::from(zone(1)), ReleaseOptions::NORMAL)
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .release_remote_for_caller(CallerZone::from(zone(1)), ReleaseOptions::OPTIMISTIC)
                .error_code,
            crate::OK()
        );
        assert!(
            marshaller
                .releases
                .lock()
                .expect("releases mutex poisoned")
                .is_empty()
        );

        assert_eq!(
            object_proxy
                .release_remote_for_caller(CallerZone::from(zone(1)), ReleaseOptions::NORMAL)
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .release_remote_for_caller(CallerZone::from(zone(1)), ReleaseOptions::OPTIMISTIC)
                .error_code,
            crate::OK()
        );
        assert_eq!(
            object_proxy
                .release_remote_for_caller(CallerZone::from(zone(2)), ReleaseOptions::NORMAL)
                .error_code,
            crate::OK()
        );

        let releases = marshaller
            .releases
            .lock()
            .expect("releases mutex poisoned")
            .clone();
        assert_eq!(releases.len(), 3);
        assert_eq!(releases[0].options, ReleaseOptions::NORMAL);
        assert_eq!(releases[1].options, ReleaseOptions::OPTIMISTIC);
        assert_eq!(releases[2].options, ReleaseOptions::NORMAL);
        for release in &releases {
            assert_eq!(release.remote_object_id, zone7_object);
        }
        assert_eq!(releases[0].caller_zone_id, CallerZone::from(zone(1)));
        assert_eq!(releases[1].caller_zone_id, CallerZone::from(zone(1)));
        assert_eq!(releases[2].caller_zone_id, CallerZone::from(zone(2)));
        assert_eq!(object_proxy.shared_count(), 0);
        assert_eq!(object_proxy.optimistic_count(), 0);
    }

    #[test]
    fn remote_object_callers_release_on_final_drop_per_pointer_kind() {
        let zone5_service = Arc::new(Service::new("zone5", zone(5)));
        let marshaller = Arc::new(FakeMarshaller::default());
        let zone7_transport = Transport::new(
            "zone5-to-zone7",
            zone(5),
            zone(7),
            Arc::downgrade(&zone5_service),
            marshaller.clone(),
        );
        let zone7_object = remote_object(7, 80);
        let service_proxy = ServiceProxy::with_transport(
            zone5_service,
            zone7_transport,
            GeneratedRpcCallContext {
                protocol_version: 3,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 2,
                caller_zone_id: CallerZone::from(zone(5)),
                remote_object_id: zone7_object.clone(),
            },
        );
        let object_proxy = service_proxy
            .proxy_for_remote_object(zone7_object.clone())
            .object_proxy();
        let shared_caller = service_proxy.proxy_for_remote_object_with_ref(
            zone7_object.clone(),
            crate::InterfacePointerKind::Shared,
        );
        let optimistic_caller = service_proxy.proxy_for_remote_object_with_ref(
            zone7_object.clone(),
            crate::InterfacePointerKind::Optimistic,
        );

        assert_eq!(object_proxy.shared_count(), 1);
        assert_eq!(object_proxy.optimistic_count(), 1);
        assert!(
            marshaller
                .add_refs
                .lock()
                .expect("add_refs mutex poisoned")
                .is_empty()
        );

        drop(shared_caller);
        drop(optimistic_caller);

        let releases = marshaller
            .releases
            .lock()
            .expect("releases mutex poisoned")
            .clone();
        assert_eq!(releases.len(), 2);
        assert_eq!(releases[0].remote_object_id, zone7_object);
        assert_eq!(releases[0].caller_zone_id, CallerZone::from(zone(5)));
        assert_eq!(releases[0].options, ReleaseOptions::NORMAL);
        assert_eq!(releases[1].remote_object_id, zone7_object);
        assert_eq!(releases[1].caller_zone_id, CallerZone::from(zone(5)));
        assert_eq!(releases[1].options, ReleaseOptions::OPTIMISTIC);
        assert_eq!(object_proxy.shared_count(), 0);
        assert_eq!(object_proxy.optimistic_count(), 0);
    }
}
