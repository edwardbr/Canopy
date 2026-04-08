//! Rust counterpart of `c++/rpc/include/rpc/internal/service.h`.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, Weak};

use crate::internal::bindings::{
    BindableInterfaceValue, BoundInterface, bind_incoming_optimistic, bind_incoming_shared,
    bind_outgoing_interface,
};
use crate::internal::bindings_fwd::{
    InterfaceBindResult, InterfacePointerKind, RemoteObjectBindResult,
};
use crate::internal::error_codes;
use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
};
use crate::internal::runtime_traits::{ServiceRuntime, TransportRuntime};
use crate::internal::service_proxy::{GeneratedRpcCallContext, ServiceProxy};
use crate::internal::stub::ObjectStub;
use crate::internal::version::{HIGHEST_SUPPORTED_VERSION, LOWEST_SUPPORTED_VERSION};
use crate::rpc_types::CallerZone;
use crate::rpc_types::{Encoding, Object, ReleaseOptions, RemoteObject, Zone, ZoneAddress};

thread_local! {
    static CURRENT_SERVICE: std::cell::Cell<*const Service> = const { std::cell::Cell::new(std::ptr::null()) };
}

pub const DUMMY_OBJECT_ID: Object = Object::new(u64::MAX);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceConfig {
    pub initial_zone: Zone,
}

impl Default for ServiceConfig {
    fn default() -> Self {
        Self {
            initial_zone: Zone::from(
                ZoneAddress::create(Default::default())
                    .expect("default local zone address should be valid"),
            ),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RemoteObjectResult {
    pub error_code: i32,
    pub descriptor: RemoteObject,
}

impl RemoteObjectResult {
    pub fn new(error_code: i32, descriptor: RemoteObject) -> Self {
        Self {
            error_code,
            descriptor,
        }
    }
}

pub trait ServiceEvent: Send + Sync {
    fn on_object_released(&self, object_id: Object, destination: Zone);
}

#[derive(Debug)]
pub struct Service {
    zone_id: Zone,
    name: String,
    object_id_generator: AtomicU64,
    default_encoding: Mutex<Encoding>,
    stubs: Mutex<BTreeMap<Object, Weak<Mutex<ObjectStub>>>>,
    transports: Mutex<BTreeMap<Zone, Weak<dyn TransportRuntime>>>,
    service_proxies: Mutex<BTreeMap<Zone, Weak<ServiceProxy>>>,
    service_events: Mutex<Vec<Weak<dyn ServiceEvent>>>,
    self_weak: Mutex<Weak<Service>>,
    runtime_weak: Mutex<Option<Weak<dyn ServiceRuntime>>>,
}

impl Service {
    fn has_supported_version(&self, protocol_version: u64) -> bool {
        protocol_version >= LOWEST_SUPPORTED_VERSION
            && protocol_version <= HIGHEST_SUPPORTED_VERSION
    }

    pub fn new(name: impl Into<String>, zone_id: Zone) -> Self {
        assert!(
            zone_id.get_address().get_subnet() != 0,
            "service zone subnet must be non-zero"
        );
        Self {
            zone_id,
            name: name.into(),
            object_id_generator: AtomicU64::new(0),
            default_encoding: Mutex::new(Encoding::default()),
            stubs: Mutex::new(BTreeMap::new()),
            transports: Mutex::new(BTreeMap::new()),
            service_proxies: Mutex::new(BTreeMap::new()),
            service_events: Mutex::new(Vec::new()),
            self_weak: Mutex::new(Weak::new()),
            runtime_weak: Mutex::new(None),
        }
    }

    pub fn new_shared(name: impl Into<String>, zone_id: Zone) -> Arc<Self> {
        let service = Arc::new(Self::new(name, zone_id));
        service.attach_self(&service);
        let runtime: Arc<dyn ServiceRuntime> = service.clone();
        service.attach_runtime(&runtime);
        service
    }

    pub fn from_config(name: impl Into<String>, config: ServiceConfig) -> Self {
        Self::new(name, config.initial_zone)
    }

    pub fn attach_self(&self, service: &Arc<Self>) {
        *self.self_weak.lock().expect("self_weak mutex poisoned") = Arc::downgrade(service);
    }

    pub fn attach_runtime(&self, runtime: &Arc<dyn ServiceRuntime>) {
        *self
            .runtime_weak
            .lock()
            .expect("runtime_weak mutex poisoned") = Some(Arc::downgrade(runtime));
    }

    fn shared_from_this(&self) -> Option<Arc<Self>> {
        self.self_weak
            .lock()
            .expect("self_weak mutex poisoned")
            .upgrade()
    }

    fn shared_runtime(&self) -> Option<Arc<dyn ServiceRuntime>> {
        self.runtime_weak
            .lock()
            .expect("runtime_weak mutex poisoned")
            .as_ref()
            .and_then(Weak::upgrade)
            .or_else(|| {
                self.shared_from_this()
                    .map(|service| service as Arc<dyn ServiceRuntime>)
            })
    }

    pub fn current_service() -> Option<*const Service> {
        CURRENT_SERVICE.with(|slot| {
            let ptr = slot.get();
            (!ptr.is_null()).then_some(ptr)
        })
    }

    pub fn with_current_service<R>(f: impl FnOnce(&Service) -> R) -> Option<R> {
        CURRENT_SERVICE.with(|slot| {
            let ptr = slot.get();
            if ptr.is_null() {
                None
            } else {
                // CURRENT_SERVICE is only set by CurrentServiceGuard for the
                // duration of an active local dispatch path.
                Some(unsafe { f(&*ptr) })
            }
        })
    }

    pub fn set_current_service(service: Option<&Service>) {
        CURRENT_SERVICE
            .with(|slot| slot.set(service.map_or(std::ptr::null(), |svc| svc as *const Service)));
    }

    pub fn generate_new_object_id(&self) -> Object {
        Object::new(self.object_id_generator.fetch_add(1, Ordering::SeqCst) + 1)
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn zone_id(&self) -> Zone {
        self.zone_id.clone()
    }

    pub fn default_encoding(&self) -> Encoding {
        *self
            .default_encoding
            .lock()
            .expect("default_encoding mutex poisoned")
    }

    pub fn set_default_encoding(&self, encoding: Encoding) {
        *self
            .default_encoding
            .lock()
            .expect("default_encoding mutex poisoned") = encoding;
    }

    pub fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>) {
        self.transports
            .lock()
            .expect("transports mutex poisoned")
            .insert(destination, Arc::downgrade(&transport));
    }

    pub fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>> {
        let mut transports = self.transports.lock().expect("transports mutex poisoned");
        match transports.get(destination).and_then(Weak::upgrade) {
            Some(transport) => Some(transport),
            None => {
                transports.remove(destination);
                None
            }
        }
    }

    fn get_transport_via_requesting_zone(
        &self,
        destination: &Zone,
        requesting_zone_id: &Zone,
    ) -> Option<Arc<dyn TransportRuntime>> {
        self.get_transport(destination).or_else(|| {
            let transport = self.get_transport(requesting_zone_id)?;
            self.add_transport(destination.clone(), transport.clone());
            Some(transport)
        })
    }

    fn get_zone_proxy(
        &self,
        caller_zone_id: &CallerZone,
        destination_zone_id: &Zone,
        remote_object_id: RemoteObject,
    ) -> Option<Arc<ServiceProxy>> {
        if let Some(existing) = self
            .service_proxies
            .lock()
            .expect("service_proxies mutex poisoned")
            .get(destination_zone_id)
            .and_then(Weak::upgrade)
        {
            return Some(existing);
        }

        let transport = self
            .get_transport(destination_zone_id)
            .or_else(|| self.get_transport(caller_zone_id))?;

        let adjacent_zone_id = transport.adjacent_zone_id();
        if adjacent_zone_id == *caller_zone_id && caller_zone_id != destination_zone_id {
            self.add_transport(destination_zone_id.clone(), transport.clone());
        }

        let service = self.shared_runtime()?;
        let proxy = ServiceProxy::with_transport(
            service,
            transport,
            GeneratedRpcCallContext {
                protocol_version: HIGHEST_SUPPORTED_VERSION,
                encoding_type: self.default_encoding(),
                tag: 0,
                caller_zone_id: self.zone_id.clone(),
                remote_object_id,
            },
        );
        self.service_proxies
            .lock()
            .expect("service_proxies mutex poisoned")
            .insert(destination_zone_id.clone(), Arc::downgrade(&proxy));
        Some(proxy)
    }

    pub fn check_is_empty(&self) -> bool {
        self.stubs
            .lock()
            .expect("stubs mutex poisoned")
            .values()
            .all(|stub| stub.strong_count() == 0)
    }

    pub fn get_object(&self, object_id: Object) -> Option<Arc<Mutex<ObjectStub>>> {
        self.stubs
            .lock()
            .expect("stubs mutex poisoned")
            .get(&object_id)
            .and_then(Weak::upgrade)
    }

    pub fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32 {
        let object_id = stub.lock().expect("object stub mutex poisoned").id();
        let mut stubs = self.stubs.lock().expect("stubs mutex poisoned");
        if stubs.get(&object_id).and_then(Weak::upgrade).is_some() {
            return error_codes::REFERENCE_COUNT_ERROR();
        }

        stubs.insert(object_id, Arc::downgrade(stub));
        {
            let mut guard = stub.lock().expect("object stub mutex poisoned");
            guard.attach_to_service(self);
            guard.keep_self_alive(stub);
            guard.attach_to_target(stub);
        }
        error_codes::OK()
    }

    pub fn register_local_object<T>(
        &self,
        object_id: Object,
        target: Arc<T>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        let stub = Arc::new(Mutex::new(ObjectStub::with_target(object_id, target)));
        let error_code = self.register_stub(&stub);
        if error_codes::is_critical(error_code) {
            return Err(error_code);
        }
        Ok(stub)
    }

    pub fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32> {
        let stub = Arc::new(Mutex::new(ObjectStub::with_rpc_object(object_id, target)));
        let error_code = self.register_stub(&stub);
        if error_codes::is_critical(error_code) {
            return Err(error_code);
        }
        Ok(stub)
    }

    pub fn lookup_local_interface<T>(&self, object_id: Object) -> Result<Arc<T>, i32>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        let Some(stub) = self.get_object(object_id) else {
            return Err(error_codes::OBJECT_NOT_FOUND());
        };

        stub.lock()
            .expect("object stub mutex poisoned")
            .get_local_interface::<T>(crate::rpc_types::InterfaceOrdinal::new(T::get_id(
                crate::version::get_version(),
            )))
            .ok_or(error_codes::INVALID_INTERFACE_ID())
    }

    pub fn lookup_local_interface_view<T>(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<T>, i32>
    where
        T: crate::internal::CastingInterface + ?Sized,
    {
        let Some(stub) = self.get_object(object_id) else {
            return Err(error_codes::OBJECT_NOT_FOUND());
        };

        stub.lock()
            .expect("object stub mutex poisoned")
            .get_local_interface_view::<T>(interface_id)
            .ok_or(error_codes::INVALID_INTERFACE_ID())
    }

    pub fn lookup_local_interface_view_erased(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<dyn std::any::Any + Send + Sync>, i32> {
        let Some(stub) = self.get_object(object_id) else {
            return Err(error_codes::OBJECT_NOT_FOUND());
        };

        stub.lock()
            .expect("object stub mutex poisoned")
            .get_local_interface_erased(interface_id)
            .ok_or(error_codes::INVALID_INTERFACE_ID())
    }

    pub fn bind_incoming_shared_interface<T>(
        &self,
        encap: &RemoteObject,
    ) -> InterfaceBindResult<Arc<T>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        self.bind_incoming_shared_interface_from(self.zone_id.clone(), encap)
    }

    pub fn bind_incoming_shared_interface_from<T>(
        &self,
        caller_zone_id: CallerZone,
        encap: &RemoteObject,
    ) -> InterfaceBindResult<Arc<T>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        bind_incoming_shared(
            &self.zone_id,
            encap,
            |object_id| self.lookup_local_interface::<T>(object_id),
            |remote| {
                let Some(service_proxy) =
                    self.get_zone_proxy(&caller_zone_id, &remote.as_zone(), remote.clone())
                else {
                    return InterfaceBindResult::null(error_codes::ZONE_NOT_FOUND());
                };
                let object_proxy = service_proxy.get_or_create_object_proxy(remote.clone());
                let add_ref = object_proxy
                    .add_ref_remote_for_caller(self.zone_id.clone(), InterfacePointerKind::Shared);
                if error_codes::is_critical(add_ref.error_code) {
                    return InterfaceBindResult::null(add_ref.error_code);
                }
                let caller = service_proxy.proxy_for_remote_object(remote.clone());
                InterfaceBindResult::remote(
                    error_codes::OK(),
                    Arc::new(
                        <T as crate::internal::GeneratedRustInterface>::create_remote_proxy(caller),
                    ),
                )
            },
        )
    }

    pub fn bind_incoming_optimistic_interface<T>(
        &self,
        encap: &RemoteObject,
    ) -> InterfaceBindResult<crate::internal::LocalProxy<T>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        self.bind_incoming_optimistic_interface_from(self.zone_id.clone(), encap)
    }

    pub fn bind_incoming_optimistic_interface_from<T>(
        &self,
        caller_zone_id: CallerZone,
        encap: &RemoteObject,
    ) -> InterfaceBindResult<crate::internal::LocalProxy<T>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        bind_incoming_optimistic(
            &self.zone_id,
            encap,
            |object_id| self.lookup_local_interface::<T>(object_id),
            |remote| {
                let Some(service_proxy) =
                    self.get_zone_proxy(&caller_zone_id, &remote.as_zone(), remote.clone())
                else {
                    return InterfaceBindResult::null(error_codes::ZONE_NOT_FOUND());
                };
                let object_proxy = service_proxy.get_or_create_object_proxy(remote.clone());
                let add_ref = object_proxy.add_ref_remote_for_caller(
                    self.zone_id.clone(),
                    InterfacePointerKind::Optimistic,
                );
                if error_codes::is_critical(add_ref.error_code) {
                    return InterfaceBindResult::null(add_ref.error_code);
                }
                let caller = service_proxy.proxy_for_remote_object(remote.clone());
                let proxy = Arc::new(
                    <T as crate::internal::GeneratedRustInterface>::create_remote_proxy(caller),
                );
                InterfaceBindResult::remote(
                    error_codes::OK(),
                    crate::internal::LocalProxy::from_remote(proxy),
                )
            },
        )
    }

    pub fn find_local_stub_for_interface<T>(&self, iface: &Arc<T>) -> Option<Arc<Mutex<ObjectStub>>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        let stubs: Vec<Arc<Mutex<ObjectStub>>> = self
            .stubs
            .lock()
            .expect("stubs mutex poisoned")
            .values()
            .filter_map(Weak::upgrade)
            .collect();

        for stub in stubs {
            let matches = {
                let guard = stub.lock().expect("object stub mutex poisoned");
                guard
                    .get_local_interface::<T>(crate::rpc_types::InterfaceOrdinal::new(T::get_id(
                        crate::version::get_version(),
                    )))
                    .is_some_and(|candidate| Arc::ptr_eq(&candidate, iface))
            };
            if matches {
                return Some(stub);
            }
        }

        None
    }

    pub fn find_local_stub_for_rpc_object(
        &self,
        iface: &Arc<dyn crate::internal::RpcObject>,
    ) -> Option<Arc<Mutex<ObjectStub>>> {
        let stubs: Vec<Arc<Mutex<ObjectStub>>> = self
            .stubs
            .lock()
            .expect("stubs mutex poisoned")
            .values()
            .filter_map(Weak::upgrade)
            .collect();

        for stub in stubs {
            let matches = {
                let guard = stub.lock().expect("object stub mutex poisoned");
                guard
                    .dispatch_target()
                    .is_some_and(|candidate| Arc::ptr_eq(&candidate, iface))
            };
            if matches {
                return Some(stub);
            }
        }

        None
    }

    pub fn get_descriptor_from_local_interface<T>(
        &self,
        caller_zone_id: CallerZone,
        iface: Arc<T>,
        optimistic: bool,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        let stub = if let Some(stub) = self.find_local_stub_for_interface(&iface) {
            if optimistic
                && stub
                    .lock()
                    .expect("object stub mutex poisoned")
                    .shared_count()
                    == 0
            {
                return RemoteObjectBindResult::new(
                    error_codes::OBJECT_GONE(),
                    None,
                    RemoteObject::default(),
                );
            }
            stub
        } else if optimistic {
            return RemoteObjectBindResult::new(
                error_codes::OBJECT_GONE(),
                None,
                RemoteObject::default(),
            );
        } else {
            let object_id = self.generate_new_object_id();
            match self.register_local_object(object_id, iface) {
                Ok(stub) => stub,
                Err(error_code) => {
                    return RemoteObjectBindResult::new(error_code, None, RemoteObject::default());
                }
            }
        };

        self.get_descriptor_from_local_stub(caller_zone_id, stub, optimistic)
    }

    fn get_descriptor_from_local_stub(
        &self,
        caller_zone_id: CallerZone,
        stub: Arc<Mutex<ObjectStub>>,
        optimistic: bool,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>> {
        {
            let mut guard = stub.lock().expect("object stub mutex poisoned");
            guard.add_ref(
                if optimistic {
                    InterfacePointerKind::Optimistic
                } else {
                    InterfacePointerKind::Shared
                },
                caller_zone_id,
            );
        }

        let descriptor = match self
            .zone_id
            .with_object(stub.lock().expect("object stub mutex poisoned").id())
        {
            Ok(descriptor) => descriptor,
            Err(_) => {
                return RemoteObjectBindResult::new(
                    error_codes::INVALID_DATA(),
                    None,
                    RemoteObject::default(),
                );
            }
        };

        RemoteObjectBindResult::new(error_codes::OK(), Some(stub), descriptor)
    }

    pub fn get_descriptor_from_local_rpc_object(
        &self,
        caller_zone_id: CallerZone,
        iface: Arc<dyn crate::internal::RpcObject>,
        optimistic: bool,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>> {
        let stub = if let Some(stub) = crate::internal::get_object_stub(iface.as_ref())
            .or_else(|| self.find_local_stub_for_rpc_object(&iface))
        {
            if optimistic
                && stub
                    .lock()
                    .expect("object stub mutex poisoned")
                    .shared_count()
                    == 0
            {
                return RemoteObjectBindResult::new(
                    error_codes::OBJECT_GONE(),
                    None,
                    RemoteObject::default(),
                );
            }
            stub
        } else if optimistic {
            return RemoteObjectBindResult::new(
                error_codes::OBJECT_GONE(),
                None,
                RemoteObject::default(),
            );
        } else {
            let object_id = self.generate_new_object_id();
            match self.register_rpc_object(object_id, iface) {
                Ok(stub) => stub,
                Err(error_code) => {
                    return RemoteObjectBindResult::new(error_code, None, RemoteObject::default());
                }
            }
        };

        self.get_descriptor_from_local_stub(caller_zone_id, stub, optimistic)
    }

    pub fn bind_outgoing_local_interface<T>(
        &self,
        caller_zone_id: CallerZone,
        iface: &BoundInterface<Arc<T>>,
        pointer_kind: InterfacePointerKind,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        bind_outgoing_interface(
            iface,
            pointer_kind,
            |iface, pointer_kind| {
                self.get_descriptor_from_local_interface(
                    caller_zone_id,
                    iface.clone(),
                    pointer_kind.is_optimistic(),
                )
            },
            |_iface, _pointer_kind| {
                RemoteObjectBindResult::new(
                    error_codes::TRANSPORT_ERROR(),
                    None,
                    RemoteObject::default(),
                )
            },
        )
    }

    pub fn bind_outgoing_local_optimistic_interface<T>(
        &self,
        caller_zone_id: CallerZone,
        iface: &BoundInterface<crate::internal::LocalProxy<T>>,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        bind_outgoing_interface(
            iface,
            InterfacePointerKind::Optimistic,
            |iface, _pointer_kind| {
                let Some(shared) = iface.upgrade() else {
                    return RemoteObjectBindResult::new(
                        error_codes::OBJECT_GONE(),
                        None,
                        RemoteObject::default(),
                    );
                };
                self.get_descriptor_from_local_interface(caller_zone_id, shared, true)
            },
            |_iface, _pointer_kind| {
                RemoteObjectBindResult::new(
                    error_codes::TRANSPORT_ERROR(),
                    None,
                    RemoteObject::default(),
                )
            },
        )
    }

    pub fn bind_outgoing_local_erased_interface<T>(
        &self,
        caller_zone_id: CallerZone,
        iface: &BoundInterface<Arc<T>>,
        pointer_kind: InterfacePointerKind,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
    where
        T: crate::internal::GeneratedRustInterface + ?Sized,
    {
        let caller_zone_for_local = caller_zone_id.clone();
        let caller_zone_for_remote = caller_zone_id;
        bind_outgoing_interface(
            iface,
            pointer_kind,
            |iface, pointer_kind| {
                let Some(stub) = crate::internal::get_object_stub(iface.as_ref()) else {
                    return RemoteObjectBindResult::new(
                        error_codes::INVALID_DATA(),
                        None,
                        RemoteObject::default(),
                    );
                };
                self.get_descriptor_from_local_stub(
                    caller_zone_for_local,
                    stub,
                    pointer_kind.is_optimistic(),
                )
            },
            |iface, pointer_kind| {
                let Some(descriptor) = BindableInterfaceValue::remote_object_id(iface) else {
                    return RemoteObjectBindResult::new(
                        error_codes::INVALID_DATA(),
                        None,
                        RemoteObject::default(),
                    );
                };
                let Some(object_proxy) = BindableInterfaceValue::remote_object_proxy(iface) else {
                    return RemoteObjectBindResult::new(
                        error_codes::OBJECT_GONE(),
                        None,
                        RemoteObject::default(),
                    );
                };
                let add_ref =
                    object_proxy.add_ref_remote_for_caller(caller_zone_for_remote, pointer_kind);
                RemoteObjectBindResult::new(add_ref.error_code, None, descriptor)
            },
        )
    }

    pub fn bind_outgoing_local_optimistic_erased_interface<T>(
        &self,
        caller_zone_id: CallerZone,
        iface: &BoundInterface<crate::internal::LocalProxy<T>>,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
    where
        T: crate::internal::GeneratedRustInterface + ?Sized,
    {
        let caller_zone_for_local = caller_zone_id.clone();
        let caller_zone_for_remote = caller_zone_id;
        bind_outgoing_interface(
            iface,
            InterfacePointerKind::Optimistic,
            |iface, _pointer_kind| {
                let Some(shared) = iface.upgrade() else {
                    return RemoteObjectBindResult::new(
                        error_codes::OBJECT_GONE(),
                        None,
                        RemoteObject::default(),
                    );
                };
                let Some(stub) = crate::internal::get_object_stub(shared.as_ref()) else {
                    return RemoteObjectBindResult::new(
                        error_codes::INVALID_DATA(),
                        None,
                        RemoteObject::default(),
                    );
                };
                self.get_descriptor_from_local_stub(caller_zone_for_local, stub, true)
            },
            |iface, pointer_kind| {
                let Some(descriptor) = BindableInterfaceValue::remote_object_id(iface) else {
                    return RemoteObjectBindResult::new(
                        error_codes::INVALID_DATA(),
                        None,
                        RemoteObject::default(),
                    );
                };
                let Some(object_proxy) = BindableInterfaceValue::remote_object_proxy(iface) else {
                    return RemoteObjectBindResult::new(
                        error_codes::OBJECT_GONE(),
                        None,
                        RemoteObject::default(),
                    );
                };
                let add_ref =
                    object_proxy.add_ref_remote_for_caller(caller_zone_for_remote, pointer_kind);
                RemoteObjectBindResult::new(add_ref.error_code, None, descriptor)
            },
        )
    }

    pub fn bind_outgoing_local_rpc_object_interface(
        &self,
        caller_zone_id: CallerZone,
        iface: &BoundInterface<Arc<dyn crate::internal::RpcObject>>,
        pointer_kind: InterfacePointerKind,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>> {
        bind_outgoing_interface(
            iface,
            pointer_kind,
            |iface, pointer_kind| {
                self.get_descriptor_from_local_rpc_object(
                    caller_zone_id,
                    iface.clone(),
                    pointer_kind.is_optimistic(),
                )
            },
            |_iface, _pointer_kind| {
                RemoteObjectBindResult::new(
                    error_codes::TRANSPORT_ERROR(),
                    None,
                    RemoteObject::default(),
                )
            },
        )
    }

    pub fn bind_outgoing_local_optimistic_rpc_object_interface(
        &self,
        caller_zone_id: CallerZone,
        iface: &BoundInterface<crate::internal::LocalProxy<dyn crate::internal::RpcObject>>,
    ) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>> {
        bind_outgoing_interface(
            iface,
            InterfacePointerKind::Optimistic,
            |iface, _pointer_kind| {
                let Some(shared) = iface.upgrade() else {
                    return RemoteObjectBindResult::new(
                        error_codes::OBJECT_GONE(),
                        None,
                        RemoteObject::default(),
                    );
                };
                self.get_descriptor_from_local_rpc_object(caller_zone_id, shared, true)
            },
            |_iface, _pointer_kind| {
                RemoteObjectBindResult::new(
                    error_codes::TRANSPORT_ERROR(),
                    None,
                    RemoteObject::default(),
                )
            },
        )
    }

    pub fn remove_stub(&self, object_id: Object) -> Option<Arc<Mutex<ObjectStub>>> {
        let removed = self
            .stubs
            .lock()
            .expect("stubs mutex poisoned")
            .remove(&object_id)
            .and_then(|stub| stub.upgrade());
        if let Some(stub) = &removed {
            let mut guard = stub.lock().expect("object stub mutex poisoned");
            guard.detach_from_target();
            guard.detach_from_service();
            guard.dont_keep_alive();
        }
        removed
    }

    pub fn add_service_event(&self, event: Weak<dyn ServiceEvent>) {
        self.service_events
            .lock()
            .expect("service_events mutex poisoned")
            .push(event);
    }

    pub fn remove_service_event(&self, event: &Weak<dyn ServiceEvent>) {
        self.service_events
            .lock()
            .expect("service_events mutex poisoned")
            .retain(|candidate| !Weak::ptr_eq(candidate, event));
    }

    pub fn notify_object_gone_event(&self, object_id: Object, destination: Zone) {
        let listeners = {
            let mut listeners = self
                .service_events
                .lock()
                .expect("service_events mutex poisoned");
            listeners.retain(|listener| listener.strong_count() > 0);
            listeners.clone()
        };

        for listener in listeners {
            if let Some(listener) = listener.upgrade() {
                listener.on_object_released(object_id, destination.clone());
            }
        }
    }

    pub fn release(&self, params: ReleaseParams) -> StandardResult {
        if !self.has_supported_version(params.protocol_version) {
            return StandardResult::new(error_codes::INVALID_VERSION(), vec![]);
        }

        if !params
            .remote_object_id
            .get_address()
            .same_zone(self.zone_id.get_address())
        {
            return self
                .get_transport(&params.remote_object_id.as_zone())
                .map(|transport| {
                    self.shared_runtime()
                        .map(|runtime| {
                            runtime
                                .outbound_release_via_transport(params.clone(), transport.clone())
                        })
                        .unwrap_or_else(|| transport.release(params))
                })
                .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]));
        }

        let object_id = params.remote_object_id.get_object_id();
        let Some(stub) = self.get_object(object_id) else {
            return StandardResult::new(error_codes::OBJECT_NOT_FOUND(), vec![]);
        };

        self.outbound_release(params, stub)
    }

    pub fn object_released(&self, params: ObjectReleasedParams) {
        if !self.has_supported_version(params.protocol_version) {
            return;
        }

        self.notify_object_gone_event(
            params.remote_object_id.get_object_id(),
            params.remote_object_id.as_zone(),
        );
    }

    pub fn release_local_stub(
        &self,
        stub: Arc<Mutex<ObjectStub>>,
        is_optimistic: bool,
        caller_zone_id: CallerZone,
    ) -> u64 {
        let (count, object_id, optimistic_refs) = {
            let mut guard = stub.lock().expect("object stub mutex poisoned");
            let count = guard.release(
                if is_optimistic {
                    ReleaseOptions::OPTIMISTIC
                } else {
                    ReleaseOptions::NORMAL
                },
                caller_zone_id,
            );
            let object_id = guard.id();
            let optimistic_refs = if count == 0 && !is_optimistic {
                guard.zones_with_optimistic_refs()
            } else {
                Vec::new()
            };
            (count, object_id, optimistic_refs)
        };

        if count == 0 && !is_optimistic {
            self.remove_stub(object_id);
            for zone in optimistic_refs {
                self.notify_object_gone_event(object_id, Zone::from(zone.get_address().clone()));
            }
        }

        count
    }

    pub fn transport_down(&self, caller_zone_id: CallerZone, destination_zone_id: Zone) {
        let stub_ids: Vec<Object> = self
            .stubs
            .lock()
            .expect("stubs mutex poisoned")
            .iter()
            .filter_map(|(object_id, stub)| {
                let stub = stub.upgrade()?;
                let has_refs = stub
                    .lock()
                    .expect("object stub mutex poisoned")
                    .has_references_from_zone(caller_zone_id.clone());
                has_refs.then_some(*object_id)
            })
            .collect();

        let mut removed = Vec::new();
        for object_id in stub_ids {
            if let Some(stub) = self.get_object(object_id) {
                let should_delete = stub
                    .lock()
                    .expect("object stub mutex poisoned")
                    .release_all_from_zone(caller_zone_id.clone());
                if should_delete {
                    self.remove_stub(object_id);
                    removed.push(object_id);
                }
            }
        }

        for object_id in removed {
            self.notify_object_gone_event(object_id, destination_zone_id.clone());
        }
    }

    pub fn transport_down_from_params(&self, params: TransportDownParams) {
        if !self.has_supported_version(params.protocol_version) {
            return;
        }

        self.transport_down(params.caller_zone_id, params.destination_zone_id.into());
    }

    pub fn send(&self, params: SendParams) -> SendResult {
        if !self.has_supported_version(params.protocol_version) {
            return SendResult::new(error_codes::INVALID_VERSION(), vec![], vec![]);
        }

        if !params
            .remote_object_id
            .get_address()
            .same_zone(self.zone_id.get_address())
        {
            return self
                .get_transport(&params.remote_object_id.as_zone())
                .map(|transport| {
                    self.shared_runtime()
                        .map(|runtime| {
                            runtime.outbound_send_via_transport(params.clone(), transport.clone())
                        })
                        .unwrap_or_else(|| transport.send(params))
                })
                .unwrap_or_else(|| SendResult::new(error_codes::ZONE_NOT_FOUND(), vec![], vec![]));
        }

        let object_id = params.remote_object_id.get_object_id();
        let Some(stub) = self.get_object(object_id) else {
            return SendResult::new(error_codes::OBJECT_NOT_FOUND(), vec![], vec![]);
        };

        self.outbound_send(params, stub)
    }

    pub fn post(&self, params: PostParams) {
        if !self.has_supported_version(params.protocol_version) {
            return;
        }

        if !params
            .remote_object_id
            .get_address()
            .same_zone(self.zone_id.get_address())
        {
            if let Some(transport) = self.get_transport(&params.remote_object_id.as_zone()) {
                if let Some(runtime) = self.shared_runtime() {
                    runtime.outbound_post_via_transport(params, transport);
                } else {
                    transport.post(params);
                }
            }
            return;
        }

        let object_id = params.remote_object_id.get_object_id();
        let Some(stub) = self.get_object(object_id) else {
            return;
        };

        self.outbound_post(params, stub);
    }

    pub fn try_cast(&self, params: TryCastParams) -> StandardResult {
        if !self.has_supported_version(params.protocol_version) {
            return StandardResult::new(error_codes::INVALID_VERSION(), vec![]);
        }

        if !params
            .remote_object_id
            .get_address()
            .same_zone(self.zone_id.get_address())
        {
            return self
                .get_transport(&params.remote_object_id.as_zone())
                .map(|transport| {
                    self.shared_runtime()
                        .map(|runtime| {
                            runtime
                                .outbound_try_cast_via_transport(params.clone(), transport.clone())
                        })
                        .unwrap_or_else(|| transport.try_cast(params))
                })
                .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]));
        }

        let object_id = params.remote_object_id.get_object_id();
        let Some(stub) = self.get_object(object_id) else {
            return StandardResult::new(error_codes::OBJECT_NOT_FOUND(), vec![]);
        };

        self.outbound_try_cast(params, stub)
    }

    pub fn add_ref(&self, params: AddRefParams) -> StandardResult {
        if !self.has_supported_version(params.protocol_version) {
            return StandardResult::new(error_codes::INVALID_VERSION(), vec![]);
        }

        let remote_zone = params.remote_object_id.as_zone();
        let object_id = params.remote_object_id.get_object_id();
        let optimistic = !(params.build_out_param_channel
            & crate::rpc_types::AddRefOptions::OPTIMISTIC)
            .is_empty();
        let build_caller_channel = !(params.build_out_param_channel
            & crate::rpc_types::AddRefOptions::BUILD_CALLER_ROUTE)
            .is_empty();
        let build_dest_channel = !(params.build_out_param_channel
            & crate::rpc_types::AddRefOptions::BUILD_DESTINATION_ROUTE)
            .is_empty()
            || params.build_out_param_channel == crate::rpc_types::AddRefOptions::NORMAL
            || params.build_out_param_channel == crate::rpc_types::AddRefOptions::OPTIMISTIC;

        if build_caller_channel {
            if self.zone_id != params.caller_zone_id {
                let Some(caller_transport) = self.get_transport_via_requesting_zone(
                    &params.caller_zone_id,
                    &params.requesting_zone_id,
                ) else {
                    return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
                };
                let mut caller_params = params.clone();
                caller_params.requesting_zone_id = self.zone_id.clone();
                caller_params.build_out_param_channel =
                    crate::rpc_types::AddRefOptions::BUILD_CALLER_ROUTE
                        | if optimistic {
                            crate::rpc_types::AddRefOptions::OPTIMISTIC
                        } else {
                            crate::rpc_types::AddRefOptions::NORMAL
                        };
                let caller_result = if let Some(runtime) = self.shared_runtime() {
                    runtime.outbound_add_ref_via_transport(caller_params, caller_transport)
                } else {
                    caller_transport.add_ref(caller_params)
                };
                if caller_result.error_code != error_codes::OK() {
                    return caller_result;
                }
            } else if self
                .get_transport_via_requesting_zone(&remote_zone, &params.requesting_zone_id)
                .is_none()
            {
                return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
            }
        }

        if build_dest_channel {
            if !params
                .remote_object_id
                .get_address()
                .same_zone(self.zone_id.get_address())
            {
                let mut dest_params = params.clone();
                dest_params.build_out_param_channel = params.build_out_param_channel
                    & !crate::rpc_types::AddRefOptions::BUILD_CALLER_ROUTE;
                return self
                    .get_transport_via_requesting_zone(&remote_zone, &params.requesting_zone_id)
                    .map(|transport| {
                        self.shared_runtime()
                            .map(|runtime| {
                                runtime.outbound_add_ref_via_transport(
                                    dest_params.clone(),
                                    transport.clone(),
                                )
                            })
                            .unwrap_or_else(|| transport.add_ref(dest_params))
                    })
                    .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]));
            }

            if object_id == DUMMY_OBJECT_ID {
                return StandardResult::new(error_codes::OK(), vec![]);
            }

            let Some(stub) = self.get_object(object_id) else {
                return StandardResult::new(error_codes::OBJECT_NOT_FOUND(), vec![]);
            };
            self.get_transport_via_requesting_zone(
                &params.caller_zone_id,
                &params.requesting_zone_id,
            );
            return self.outbound_add_ref(params, stub);
        }

        StandardResult::new(error_codes::OK(), vec![])
    }

    pub fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        if !self.has_supported_version(params.protocol_version) {
            return NewZoneIdResult::new(error_codes::INVALID_VERSION(), Zone::default(), vec![]);
        }

        self.outbound_get_new_zone_id(params)
    }

    pub fn outbound_send(&self, params: SendParams, stub: Arc<Mutex<ObjectStub>>) -> SendResult {
        let _ = self;
        let target = stub
            .lock()
            .expect("object stub mutex poisoned")
            .dispatch_target();

        if let Some(target) = target {
            return target.__rpc_call(params);
        }

        SendResult::new(error_codes::INVALID_INTERFACE_ID(), vec![], vec![])
    }

    pub fn outbound_post(&self, params: PostParams, stub: Arc<Mutex<ObjectStub>>) {
        let _ = self.outbound_send(
            SendParams {
                protocol_version: params.protocol_version,
                encoding_type: params.encoding_type,
                tag: params.tag,
                caller_zone_id: params.caller_zone_id,
                remote_object_id: params.remote_object_id,
                interface_id: params.interface_id,
                method_id: params.method_id,
                in_data: params.in_data,
                in_back_channel: params.in_back_channel,
            },
            stub,
        );
    }

    pub fn outbound_try_cast(
        &self,
        params: TryCastParams,
        stub: Arc<Mutex<ObjectStub>>,
    ) -> StandardResult {
        let _ = self;
        StandardResult::new(
            stub.lock()
                .expect("object stub mutex poisoned")
                .try_cast(params.interface_id),
            vec![],
        )
    }

    pub fn outbound_add_ref(
        &self,
        params: AddRefParams,
        stub: Arc<Mutex<ObjectStub>>,
    ) -> StandardResult {
        let _ = self;
        let pointer_kind = if !(params.build_out_param_channel
            & crate::rpc_types::AddRefOptions::OPTIMISTIC)
            .is_empty()
        {
            crate::internal::InterfacePointerKind::Optimistic
        } else {
            crate::internal::InterfacePointerKind::Shared
        };
        stub.lock()
            .expect("object stub mutex poisoned")
            .add_ref(pointer_kind, params.caller_zone_id);
        StandardResult::new(error_codes::OK(), vec![])
    }

    pub fn outbound_release(
        &self,
        params: ReleaseParams,
        stub: Arc<Mutex<ObjectStub>>,
    ) -> StandardResult {
        self.release_local_stub(
            stub,
            params.options == ReleaseOptions::OPTIMISTIC,
            params.caller_zone_id,
        );

        StandardResult::new(error_codes::OK(), vec![])
    }

    pub fn outbound_get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
        NewZoneIdResult::new(error_codes::ZONE_NOT_SUPPORTED(), Zone::default(), vec![])
    }
}

impl IMarshaller for Service {
    fn send(&self, params: SendParams) -> SendResult {
        Service::send(self, params)
    }

    fn post(&self, params: PostParams) {
        Service::post(self, params);
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        Service::try_cast(self, params)
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        Service::add_ref(self, params)
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        Service::release(self, params)
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        Service::object_released(self, params);
    }

    fn transport_down(&self, params: TransportDownParams) {
        self.transport_down_from_params(params);
    }

    fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
        Service::get_new_zone_id(self, _params)
    }
}

impl ServiceRuntime for Service {
    fn name(&self) -> &str {
        Service::name(self)
    }

    fn zone_id(&self) -> Zone {
        Service::zone_id(self)
    }

    fn default_encoding(&self) -> Encoding {
        Service::default_encoding(self)
    }

    fn generate_new_object_id(&self) -> Object {
        Service::generate_new_object_id(self)
    }

    fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>) {
        Service::add_transport(self, destination, transport);
    }

    fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>> {
        Service::get_transport(self, destination)
    }

    fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32 {
        Service::register_stub(self, stub)
    }

    fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32> {
        Service::register_rpc_object(self, object_id, target)
    }

    fn lookup_local_interface_view_erased(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<dyn std::any::Any + Send + Sync>, i32> {
        let Some(stub) = self.get_object(object_id) else {
            return Err(error_codes::OBJECT_NOT_FOUND());
        };

        stub.lock()
            .expect("object stub mutex poisoned")
            .get_local_interface_erased(interface_id)
            .ok_or(error_codes::INVALID_INTERFACE_ID())
    }

    fn transport_down_from_params(&self, params: TransportDownParams) {
        Service::transport_down_from_params(self, params);
    }

    fn concrete_service(&self) -> Option<&Service> {
        Some(self)
    }
}

#[derive(Debug)]
pub struct RootService {
    service: Arc<Service>,
    next_zone_subnet: AtomicU64,
}

impl RootService {
    pub fn new_shared(name: impl Into<String>, zone_id: Zone) -> Arc<Self> {
        let service = Arc::new(Service::new(name, zone_id.clone()));
        service.attach_self(&service);
        let next_zone_subnet = zone_id.get_subnet().saturating_add(1);
        let root = Arc::new(Self {
            service,
            next_zone_subnet: AtomicU64::new(next_zone_subnet),
        });
        let runtime: Arc<dyn ServiceRuntime> = root.clone();
        root.service.attach_runtime(&runtime);
        root
    }

    pub fn from_config(name: impl Into<String>, config: ServiceConfig) -> Arc<Self> {
        Self::new_shared(name, config.initial_zone)
    }

    #[doc(hidden)]
    pub fn service(&self) -> &Arc<Service> {
        &self.service
    }

    pub fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>) {
        self.service.add_transport(destination, transport);
    }

    pub fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>> {
        self.service.get_transport(destination)
    }

    pub fn generate_new_object_id(&self) -> Object {
        self.service.generate_new_object_id()
    }

    pub fn zone_id(&self) -> Zone {
        self.service.zone_id()
    }

    pub fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32> {
        self.service.register_rpc_object(object_id, target)
    }

    pub fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32 {
        self.service.register_stub(stub)
    }

    pub fn register_local_object<T>(
        &self,
        object_id: Object,
        target: Arc<T>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        self.service.register_local_object(object_id, target)
    }

    pub fn lookup_local_interface_view<T>(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<T>, i32>
    where
        T: crate::internal::CastingInterface + ?Sized,
    {
        self.service
            .lookup_local_interface_view::<T>(object_id, interface_id)
    }

    pub fn send(&self, params: SendParams) -> SendResult {
        self.service.send(params)
    }

    pub fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.service.try_cast(params)
    }

    pub fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.service.add_ref(params)
    }

    pub fn release(&self, params: ReleaseParams) -> StandardResult {
        self.service.release(params)
    }
}

impl IMarshaller for RootService {
    fn send(&self, params: SendParams) -> SendResult {
        self.service.send(params)
    }

    fn post(&self, params: PostParams) {
        self.service.post(params);
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.service.try_cast(params)
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.service.add_ref(params)
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        self.service.release(params)
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        self.service.object_released(params);
    }

    fn transport_down(&self, params: TransportDownParams) {
        self.service.transport_down_from_params(params);
    }

    fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
        let subnet = self.next_zone_subnet.fetch_add(1, Ordering::SeqCst);
        let zone = Zone::from(
            ZoneAddress::create(crate::rpc_types::ZoneAddressArgs::new(
                crate::rpc_types::DefaultValues::VERSION_3,
                crate::rpc_types::AddressType::Local,
                0,
                vec![],
                crate::rpc_types::DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
                subnet,
                crate::rpc_types::DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
                0,
                vec![],
            ))
            .expect("root service should allocate valid local zone"),
        );
        NewZoneIdResult::new(error_codes::OK(), zone, vec![])
    }
}

impl ServiceRuntime for RootService {
    fn name(&self) -> &str {
        self.service.name()
    }

    fn zone_id(&self) -> Zone {
        self.service.zone_id()
    }

    fn default_encoding(&self) -> Encoding {
        self.service.default_encoding()
    }

    fn generate_new_object_id(&self) -> Object {
        self.service.generate_new_object_id()
    }

    fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>) {
        self.service.add_transport(destination, transport);
    }

    fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>> {
        self.service.get_transport(destination)
    }

    fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32 {
        self.service.register_stub(stub)
    }

    fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32> {
        self.service.register_rpc_object(object_id, target)
    }

    fn lookup_local_interface_view_erased(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<dyn std::any::Any + Send + Sync>, i32> {
        self.service
            .lookup_local_interface_view_erased(object_id, interface_id)
    }

    fn transport_down_from_params(&self, params: TransportDownParams) {
        self.service.transport_down_from_params(params);
    }

    fn concrete_service(&self) -> Option<&Service> {
        Some(self.service.as_ref())
    }
}

pub struct ChildService {
    service: Arc<Service>,
    parent_zone_id: Zone,
    parent_transport: Mutex<Option<Arc<dyn TransportRuntime>>>,
}

impl std::fmt::Debug for ChildService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ChildService")
            .field("zone_id", &self.service.zone_id())
            .field("parent_zone_id", &self.parent_zone_id)
            .field(
                "has_parent_transport",
                &self
                    .parent_transport
                    .lock()
                    .expect("child parent_transport mutex poisoned")
                    .is_some(),
            )
            .finish_non_exhaustive()
    }
}

impl ChildService {
    pub fn new_shared(name: impl Into<String>, zone_id: Zone, parent_zone_id: Zone) -> Arc<Self> {
        let service = Arc::new(Service::new(name, zone_id));
        service.attach_self(&service);
        let child = Arc::new(Self {
            service,
            parent_zone_id,
            parent_transport: Mutex::new(None),
        });
        let runtime: Arc<dyn ServiceRuntime> = child.clone();
        child.service.attach_runtime(&runtime);
        child
    }

    #[doc(hidden)]
    pub fn service(&self) -> &Arc<Service> {
        &self.service
    }

    pub fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>) {
        self.service.add_transport(destination, transport);
    }

    pub fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>> {
        self.service.get_transport(destination)
    }

    pub fn set_parent_transport(&self, parent_transport: Arc<dyn TransportRuntime>) {
        *self
            .parent_transport
            .lock()
            .expect("child parent_transport mutex poisoned") = Some(parent_transport);
    }

    pub fn get_parent_transport(&self) -> Option<Arc<dyn TransportRuntime>> {
        self.parent_transport
            .lock()
            .expect("child parent_transport mutex poisoned")
            .clone()
    }

    pub fn parent_zone_id(&self) -> Zone {
        self.parent_zone_id.clone()
    }

    pub fn generate_new_object_id(&self) -> Object {
        self.service.generate_new_object_id()
    }

    pub fn zone_id(&self) -> Zone {
        self.service.zone_id()
    }

    pub fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32> {
        self.service.register_rpc_object(object_id, target)
    }

    pub fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32 {
        self.service.register_stub(stub)
    }

    pub fn register_local_object<T>(
        &self,
        object_id: Object,
        target: Arc<T>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32>
    where
        T: crate::internal::GeneratedRustInterface,
    {
        self.service.register_local_object(object_id, target)
    }

    pub fn lookup_local_interface_view<T>(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<T>, i32>
    where
        T: crate::internal::CastingInterface + ?Sized,
    {
        self.service
            .lookup_local_interface_view::<T>(object_id, interface_id)
    }

    pub fn send(&self, params: SendParams) -> SendResult {
        self.service.send(params)
    }

    pub fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.service.try_cast(params)
    }

    pub fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.service.add_ref(params)
    }

    pub fn release(&self, params: ReleaseParams) -> StandardResult {
        self.service.release(params)
    }
}

impl IMarshaller for ChildService {
    fn send(&self, params: SendParams) -> SendResult {
        self.service.send(params)
    }

    fn post(&self, params: PostParams) {
        self.service.post(params);
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.service.try_cast(params)
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.service.add_ref(params)
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        self.service.release(params)
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        self.service.object_released(params);
    }

    fn transport_down(&self, params: TransportDownParams) {
        self.service.transport_down_from_params(params);
    }

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        self.get_parent_transport()
            .map(|transport| transport.get_new_zone_id(params))
            .unwrap_or_else(|| {
                NewZoneIdResult::new(error_codes::ZONE_NOT_FOUND(), Zone::default(), vec![])
            })
    }
}

impl ServiceRuntime for ChildService {
    fn name(&self) -> &str {
        self.service.name()
    }

    fn zone_id(&self) -> Zone {
        self.service.zone_id()
    }

    fn default_encoding(&self) -> Encoding {
        self.service.default_encoding()
    }

    fn generate_new_object_id(&self) -> Object {
        self.service.generate_new_object_id()
    }

    fn add_transport(&self, destination: Zone, transport: Arc<dyn TransportRuntime>) {
        self.service.add_transport(destination, transport);
    }

    fn get_transport(&self, destination: &Zone) -> Option<Arc<dyn TransportRuntime>> {
        self.service.get_transport(destination)
    }

    fn register_stub(&self, stub: &Arc<Mutex<ObjectStub>>) -> i32 {
        self.service.register_stub(stub)
    }

    fn register_rpc_object(
        &self,
        object_id: Object,
        target: Arc<dyn crate::internal::RpcObject>,
    ) -> Result<Arc<Mutex<ObjectStub>>, i32> {
        self.service.register_rpc_object(object_id, target)
    }

    fn lookup_local_interface_view_erased(
        &self,
        object_id: Object,
        interface_id: crate::rpc_types::InterfaceOrdinal,
    ) -> Result<Arc<dyn std::any::Any + Send + Sync>, i32> {
        self.service
            .lookup_local_interface_view_erased(object_id, interface_id)
    }

    fn transport_down_from_params(&self, params: TransportDownParams) {
        self.service.transport_down_from_params(params);
    }

    fn concrete_service(&self) -> Option<&Service> {
        Some(self.service.as_ref())
    }
}

pub struct CurrentServiceGuard {
    previous: *const Service,
}

#[doc(hidden)]
pub fn bind_incoming_shared_interface_local_from_runtime<T, ProxyT>(
    service: &dyn ServiceRuntime,
    caller_zone_id: CallerZone,
    encap: &RemoteObject,
    interface_id: crate::rpc_types::InterfaceOrdinal,
    convert_remote: impl FnOnce(Arc<ProxyT>) -> Arc<T>,
) -> InterfaceBindResult<Arc<T>>
where
    T: crate::internal::CastingInterface + ?Sized + Send + Sync + 'static,
    ProxyT: crate::internal::GeneratedRustInterface,
{
    if *encap == crate::internal::null_remote_descriptor() || !encap.is_set() {
        return InterfaceBindResult::null(error_codes::OK());
    }
    if encap.as_zone() == service.zone_id() {
        return match crate::internal::lookup_local_interface_view_from_runtime::<T>(
            service,
            encap.get_object_id(),
            interface_id,
        ) {
            Ok(iface) => InterfaceBindResult::local(error_codes::OK(), iface),
            Err(error_code) if error_code == error_codes::OBJECT_GONE() => {
                InterfaceBindResult::gone(
                    error_code,
                    crate::internal::InterfaceBindingOrigin::Local,
                )
            }
            Err(error_code) => InterfaceBindResult::null(error_code),
        };
    }

    let Some(concrete_service) = service.concrete_service() else {
        return InterfaceBindResult::null(error_codes::TRANSPORT_ERROR());
    };

    let binding =
        concrete_service.bind_incoming_shared_interface_from::<ProxyT>(caller_zone_id, encap);
    match binding.iface {
        crate::internal::BoundInterface::Value(proxy) => {
            InterfaceBindResult::remote(binding.error_code, convert_remote(proxy))
        }
        crate::internal::BoundInterface::Null => InterfaceBindResult::null(binding.error_code),
        crate::internal::BoundInterface::Gone => InterfaceBindResult::gone(
            binding.error_code,
            binding
                .origin
                .unwrap_or(crate::internal::InterfaceBindingOrigin::Remote),
        ),
    }
}

#[doc(hidden)]
pub fn bind_incoming_optimistic_interface_local_from_runtime<T, ProxyT>(
    service: &dyn ServiceRuntime,
    caller_zone_id: CallerZone,
    encap: &RemoteObject,
    interface_id: crate::rpc_types::InterfaceOrdinal,
    convert_remote: impl FnOnce(Arc<ProxyT>) -> Arc<T>,
) -> InterfaceBindResult<crate::internal::LocalProxy<T>>
where
    T: crate::internal::CastingInterface + ?Sized + Send + Sync + 'static,
    ProxyT: crate::internal::GeneratedRustInterface,
{
    if *encap == crate::internal::null_remote_descriptor() || !encap.is_set() {
        return InterfaceBindResult::null(error_codes::OK());
    }
    if encap.as_zone() == service.zone_id() {
        return match crate::internal::lookup_local_interface_view_from_runtime::<T>(
            service,
            encap.get_object_id(),
            interface_id,
        ) {
            Ok(iface) => InterfaceBindResult::local(
                error_codes::OK(),
                crate::internal::LocalProxy::from_remote(iface),
            ),
            Err(error_code) if error_code == error_codes::OBJECT_GONE() => {
                InterfaceBindResult::gone(
                    error_code,
                    crate::internal::InterfaceBindingOrigin::Local,
                )
            }
            Err(error_code) => InterfaceBindResult::null(error_code),
        };
    }

    let Some(concrete_service) = service.concrete_service() else {
        return InterfaceBindResult::null(error_codes::TRANSPORT_ERROR());
    };

    let binding =
        concrete_service.bind_incoming_optimistic_interface_from::<ProxyT>(caller_zone_id, encap);
    if crate::internal::is_critical(binding.error_code) {
        return InterfaceBindResult::null(binding.error_code);
    }
    match binding.iface {
        crate::internal::BoundInterface::Value(proxy) => {
            let Some(proxy) = proxy.upgrade() else {
                return InterfaceBindResult::gone(
                    error_codes::OBJECT_GONE(),
                    crate::internal::InterfaceBindingOrigin::Remote,
                );
            };
            let view = convert_remote(proxy);
            InterfaceBindResult::remote(
                binding.error_code,
                crate::internal::LocalProxy::from_remote(view),
            )
        }
        crate::internal::BoundInterface::Null => InterfaceBindResult::null(binding.error_code),
        crate::internal::BoundInterface::Gone => InterfaceBindResult::gone(
            binding.error_code,
            binding
                .origin
                .unwrap_or(crate::internal::InterfaceBindingOrigin::Remote),
        ),
    }
}

#[doc(hidden)]
pub fn bind_outgoing_local_erased_interface_from_runtime<T>(
    service: &dyn ServiceRuntime,
    caller_zone_id: CallerZone,
    iface: &crate::internal::BoundInterface<Arc<T>>,
    pointer_kind: InterfacePointerKind,
) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
where
    T: crate::internal::GeneratedRustInterface + ?Sized,
{
    let Some(concrete_service) = service.concrete_service() else {
        return RemoteObjectBindResult::new(
            error_codes::TRANSPORT_ERROR(),
            None,
            RemoteObject::default(),
        );
    };
    concrete_service.bind_outgoing_local_erased_interface(caller_zone_id, iface, pointer_kind)
}

#[doc(hidden)]
pub fn bind_outgoing_local_optimistic_erased_interface_from_runtime<T>(
    service: &dyn ServiceRuntime,
    caller_zone_id: CallerZone,
    iface: &crate::internal::BoundInterface<crate::internal::LocalProxy<T>>,
) -> RemoteObjectBindResult<Arc<Mutex<ObjectStub>>>
where
    T: crate::internal::GeneratedRustInterface + ?Sized,
{
    let Some(concrete_service) = service.concrete_service() else {
        return RemoteObjectBindResult::new(
            error_codes::TRANSPORT_ERROR(),
            None,
            RemoteObject::default(),
        );
    };
    concrete_service.bind_outgoing_local_optimistic_erased_interface(caller_zone_id, iface)
}

impl CurrentServiceGuard {
    pub fn new(service: &Service) -> Self {
        let previous = CURRENT_SERVICE.with(|slot| {
            let prev = slot.get();
            slot.set(service as *const Service);
            prev
        });
        Self { previous }
    }
}

impl Drop for CurrentServiceGuard {
    fn drop(&mut self) {
        CURRENT_SERVICE.with(|slot| slot.set(self.previous));
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex, Weak};

    use super::{
        CurrentServiceGuard, DUMMY_OBJECT_ID, RemoteObjectResult, Service, ServiceConfig,
        ServiceEvent,
    };
    use crate::internal::InterfacePointerKind;
    use crate::internal::casting_interface::CastingInterface;
    use crate::internal::error_codes;
    use crate::internal::marshaller::IMarshaller;
    use crate::internal::marshaller_params::{
        AddRefParams, GetNewZoneIdParams, ObjectReleasedParams, PostParams, ReleaseParams,
        SendParams, TransportDownParams, TryCastParams,
    };
    use crate::internal::remote_pointer::LocalProxy;
    use crate::internal::stub::ObjectStub;
    use crate::rpc_types::{
        AddRefOptions, CallerZone, Encoding, InterfaceOrdinal, Method, Object, ReleaseOptions,
        RemoteObject, Zone, ZoneAddress, ZoneAddressArgs,
    };
    use crate::version::{HIGHEST_SUPPORTED_VERSION, LOWEST_SUPPORTED_VERSION};

    fn zone(subnet: u64) -> Zone {
        let mut args = ZoneAddressArgs::default();
        args.subnet = subnet;
        Zone::from(ZoneAddress::create(args).expect("zone address"))
    }

    fn caller_zone(subnet: u64) -> CallerZone {
        CallerZone::from(zone(subnet))
    }

    #[test]
    fn service_generates_unique_object_ids_and_tracks_default_encoding() {
        let service = Service::new("svc", zone(1));
        assert_eq!(service.default_encoding(), Encoding::ProtocolBuffers);
        service.set_default_encoding(Encoding::YasBinary);
        assert_eq!(service.default_encoding(), Encoding::YasBinary);
        assert_eq!(service.generate_new_object_id(), Object::new(1));
        assert_eq!(service.generate_new_object_id(), Object::new(2));
    }

    #[test]
    fn service_tracks_current_service_with_guard() {
        let service = Service::new("svc", zone(1));
        assert!(Service::current_service().is_none());
        {
            let _guard = CurrentServiceGuard::new(&service);
            let current = Service::current_service().expect("current service");
            assert_eq!(current, &service as *const Service);
        }
        assert!(Service::current_service().is_none());
    }

    #[test]
    fn service_registers_and_removes_stubs() {
        let service = Service::new("svc", zone(1));
        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(7))));

        assert_eq!(service.register_stub(&stub), error_codes::OK());
        assert!(service.get_object(Object::new(7)).is_some());
        assert_eq!(
            service.register_stub(&stub),
            error_codes::REFERENCE_COUNT_ERROR()
        );

        let removed = service.remove_stub(Object::new(7)).expect("removed stub");
        assert_eq!(
            removed.lock().expect("stub mutex poisoned").id(),
            Object::new(7)
        );
        assert!(service.get_object(Object::new(7)).is_none());
        assert!(service.check_is_empty());
    }

    #[test]
    fn service_can_register_local_object_and_dispatch_send_and_try_cast() {
        let service = Service::new("svc", zone(1));
        service
            .register_local_object(Object::new(44), Arc::new(TestLocalObject))
            .expect("register local object");

        let send_result = service.send(SendParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            encoding_type: Encoding::ProtocolBuffers,
            tag: 0,
            caller_zone_id: caller_zone(2),
            remote_object_id: RemoteObject::from(
                zone(1).with_object(Object::new(44)).expect("object zone"),
            ),
            interface_id: InterfaceOrdinal::new(88),
            method_id: Method::new(9),
            in_data: vec![],
            in_back_channel: vec![],
        });
        assert_eq!(send_result.error_code, error_codes::OK());
        assert_eq!(send_result.out_buf, vec![9]);

        let try_cast = service.try_cast(TryCastParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            caller_zone_id: caller_zone(2),
            remote_object_id: RemoteObject::from(
                zone(1).with_object(Object::new(44)).expect("object zone"),
            ),
            interface_id: InterfaceOrdinal::new(88),
            in_back_channel: vec![],
        });
        assert_eq!(try_cast.error_code, error_codes::OK());
    }

    #[test]
    fn erased_rpc_object_descriptor_path_registers_unattached_shared_object() {
        let service = Service::new("svc", zone(1));
        let local: Arc<dyn crate::internal::RpcObject> = Arc::new(TestLocalObject);

        let descriptor = service.get_descriptor_from_local_rpc_object(caller_zone(2), local, false);

        assert_eq!(descriptor.error_code, error_codes::OK());
        assert_eq!(descriptor.descriptor.as_zone(), zone(1));
        assert_eq!(descriptor.descriptor.get_object_id(), Object::new(1));
        let stub = descriptor.stub.expect("registered stub");
        assert_eq!(stub.lock().expect("stub mutex poisoned").shared_count(), 1);
    }

    #[test]
    fn marshaller_impl_handles_add_ref_post_and_zone_request_basics() {
        let service = Service::new("svc", zone(1));
        let stub = service
            .register_local_object(Object::new(60), Arc::new(TestLocalObject))
            .expect("register local object");

        let add_ref = IMarshaller::add_ref(
            &service,
            AddRefParams {
                protocol_version: LOWEST_SUPPORTED_VERSION,
                remote_object_id: RemoteObject::from(
                    zone(1).with_object(Object::new(60)).expect("object zone"),
                ),
                caller_zone_id: caller_zone(3),
                requesting_zone_id: zone(3).into(),
                build_out_param_channel: AddRefOptions::OPTIMISTIC,
                in_back_channel: vec![],
            },
        );
        assert_eq!(add_ref.error_code, error_codes::OK());
        assert_eq!(
            stub.lock().expect("stub mutex poisoned").optimistic_count(),
            1
        );

        IMarshaller::post(
            &service,
            PostParams {
                protocol_version: LOWEST_SUPPORTED_VERSION,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 0,
                caller_zone_id: caller_zone(3),
                remote_object_id: RemoteObject::from(
                    zone(1).with_object(Object::new(60)).expect("object zone"),
                ),
                interface_id: InterfaceOrdinal::new(88),
                method_id: Method::new(10),
                in_data: vec![],
                in_back_channel: vec![],
            },
        );

        let new_zone = IMarshaller::get_new_zone_id(
            &service,
            GetNewZoneIdParams {
                protocol_version: LOWEST_SUPPORTED_VERSION,
                in_back_channel: vec![],
            },
        );
        assert_eq!(new_zone.error_code, error_codes::ZONE_NOT_SUPPORTED());
    }

    #[test]
    fn service_binds_incoming_shared_and_optimistic_interfaces_through_stub_registry() {
        let service = Service::new("svc", zone(1));
        let local = Arc::new(TestLocalObject);
        service
            .register_local_object(Object::new(71), local.clone())
            .expect("register local object");

        let encap = RemoteObject::from(zone(1).with_object(Object::new(71)).expect("object zone"));

        let shared = service.bind_incoming_shared_interface::<TestLocalObject>(&encap);
        assert_eq!(shared.error_code, error_codes::OK());
        assert!(shared.is_local());
        assert!(matches!(shared.iface, crate::BoundInterface::Value(_)));
        if let crate::BoundInterface::Value(iface) = shared.iface {
            assert!(Arc::ptr_eq(&iface, &local));
        } else {
            panic!("expected bound local shared value");
        }

        let optimistic = service.bind_incoming_optimistic_interface::<TestLocalObject>(&encap);
        assert_eq!(optimistic.error_code, error_codes::OK());
        assert!(optimistic.is_local());
        assert!(matches!(optimistic.iface, crate::BoundInterface::Value(_)));
        if let crate::BoundInterface::Value(proxy) = optimistic.iface {
            assert!(!proxy.is_null());
            assert!(!proxy.expired());
        } else {
            panic!("expected bound local optimistic value");
        }
    }

    #[test]
    fn service_bind_incoming_shared_reports_missing_and_wrong_interface() {
        let service = Service::new("svc", zone(1));
        service
            .register_local_object(Object::new(72), Arc::new(TestLocalObject))
            .expect("register local object");

        let missing = service.bind_incoming_shared_interface::<TestLocalObject>(
            &RemoteObject::from(zone(1).with_object(Object::new(999)).expect("object zone")),
        );
        assert_eq!(missing.error_code, error_codes::OBJECT_NOT_FOUND());
        assert!(matches!(missing.iface, crate::BoundInterface::Null));

        #[derive(Debug)]
        struct OtherLocalObject;

        impl crate::internal::remote_pointer::CreateLocalProxy for OtherLocalObject {
            fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
                LocalProxy::new(weak)
            }
        }

        impl CastingInterface for OtherLocalObject {
            fn __rpc_query_interface(&self, interface_id: InterfaceOrdinal) -> bool {
                interface_id == InterfaceOrdinal::new(1234)
            }
        }

        impl crate::internal::GeneratedRustInterface for OtherLocalObject {
            fn interface_name() -> &'static str {
                "test::other"
            }

            fn get_id(_rpc_version: u64) -> u64 {
                1234
            }

            fn binding_metadata() -> &'static [crate::GeneratedMethodBindingDescriptor] {
                &[]
            }
        }

        let wrong = service.bind_incoming_shared_interface::<OtherLocalObject>(
            &RemoteObject::from(zone(1).with_object(Object::new(72)).expect("object zone")),
        );
        assert_eq!(wrong.error_code, error_codes::INVALID_INTERFACE_ID());
        assert!(matches!(wrong.iface, crate::BoundInterface::Null));
    }

    #[test]
    fn outgoing_local_interface_reuses_existing_stub_and_builds_descriptor() {
        let service = Service::new("svc", zone(1));
        let local = Arc::new(TestLocalObject);
        let stub = service
            .register_local_object(Object::new(81), local.clone())
            .expect("register local object");

        let result = service.bind_outgoing_local_interface(
            caller_zone(9),
            &crate::BoundInterface::Value(local),
            crate::InterfacePointerKind::Shared,
        );

        assert_eq!(result.error_code, error_codes::OK());
        assert_eq!(result.descriptor.get_object_id(), Object::new(81));
        assert!(result.stub.is_some());
        assert_eq!(
            result
                .stub
                .as_ref()
                .expect("stub")
                .lock()
                .expect("stub mutex poisoned")
                .id(),
            Object::new(81)
        );
        assert_eq!(stub.lock().expect("stub mutex poisoned").shared_count(), 1);
    }

    #[test]
    fn outgoing_local_interface_creates_stub_for_unregistered_shared_object() {
        let service = Service::new("svc", zone(1));
        let local = Arc::new(TestLocalObject);

        let result = service.bind_outgoing_local_interface(
            caller_zone(11),
            &crate::BoundInterface::Value(local),
            crate::InterfacePointerKind::Shared,
        );

        assert_eq!(result.error_code, error_codes::OK());
        let stub = result.stub.expect("created stub");
        let guard = stub.lock().expect("stub mutex poisoned");
        assert_eq!(guard.id(), result.descriptor.get_object_id());
        assert_eq!(guard.shared_count(), 1);
        drop(guard);
        assert!(
            service
                .get_object(result.descriptor.get_object_id())
                .is_some()
        );
    }

    #[test]
    fn outgoing_local_optimistic_interface_uses_local_proxy_and_reports_gone() {
        let service = Service::new("svc", zone(1));
        let local = Arc::new(TestLocalObject);
        let shared_result = service.bind_outgoing_local_interface(
            caller_zone(12),
            &crate::BoundInterface::Value(local.clone()),
            crate::InterfacePointerKind::Shared,
        );
        assert_eq!(shared_result.error_code, error_codes::OK());
        let proxy = LocalProxy::from_shared(&local);

        let result = service.bind_outgoing_local_optimistic_interface(
            caller_zone(12),
            &crate::BoundInterface::Value(proxy),
        );

        assert_eq!(result.error_code, error_codes::OK());
        let stub = result.stub.expect("created stub");
        assert_eq!(
            stub.lock().expect("stub mutex poisoned").optimistic_count(),
            1
        );

        let unregistered = Arc::new(TestLocalObject);
        let unregistered_proxy = LocalProxy::from_shared(&unregistered);
        let unregistered_result = service.bind_outgoing_local_optimistic_interface(
            caller_zone(12),
            &crate::BoundInterface::Value(unregistered_proxy),
        );
        assert_eq!(unregistered_result.error_code, error_codes::OBJECT_GONE());
        assert!(unregistered_result.stub.is_none());

        let shared = Arc::new(TestLocalObject);
        let gone_proxy = LocalProxy::from_shared(&shared);
        drop(shared);

        let gone = service.bind_outgoing_local_optimistic_interface(
            caller_zone(12),
            &crate::BoundInterface::Value(gone_proxy),
        );
        assert_eq!(gone.error_code, error_codes::OBJECT_GONE());
        assert!(gone.stub.is_none());
    }

    #[test]
    fn outgoing_erased_rpc_object_interface_binds_shared_and_optimistic() {
        let service = Service::new("svc", zone(1));
        let local: Arc<dyn crate::internal::RpcObject> = Arc::new(TestLocalObject);

        let shared_result = service.bind_outgoing_local_rpc_object_interface(
            caller_zone(13),
            &crate::BoundInterface::Value(local.clone()),
            crate::InterfacePointerKind::Shared,
        );
        assert_eq!(shared_result.error_code, error_codes::OK());
        let object_id = shared_result.descriptor.get_object_id();

        let optimistic_proxy = LocalProxy::from_shared(&local);
        let optimistic_result = service.bind_outgoing_local_optimistic_rpc_object_interface(
            caller_zone(13),
            &crate::BoundInterface::Value(optimistic_proxy),
        );
        assert_eq!(optimistic_result.error_code, error_codes::OK());
        assert_eq!(optimistic_result.descriptor.get_object_id(), object_id);

        let stub = optimistic_result.stub.expect("erased stub");
        let guard = stub.lock().expect("stub mutex poisoned");
        assert_eq!(guard.shared_count(), 1);
        assert_eq!(guard.optimistic_count(), 1);
    }

    #[derive(Default)]
    struct RecordingEvent {
        released: Mutex<Vec<(Object, Zone)>>,
    }

    impl ServiceEvent for RecordingEvent {
        fn on_object_released(&self, object_id: Object, destination: Zone) {
            self.released
                .lock()
                .expect("released mutex poisoned")
                .push((object_id, destination));
        }
    }

    #[derive(Debug)]
    struct TestLocalObject;

    impl crate::internal::remote_pointer::CreateLocalProxy for TestLocalObject {
        fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
            LocalProxy::new(weak)
        }
    }

    impl CastingInterface for TestLocalObject {
        fn __rpc_query_interface(&self, interface_id: InterfaceOrdinal) -> bool {
            interface_id == InterfaceOrdinal::new(88)
        }

        fn __rpc_call(&self, params: SendParams) -> crate::SendResult {
            crate::SendResult::new(0, vec![params.method_id.get_val() as u8], vec![])
        }
    }

    impl crate::internal::GeneratedRustInterface for TestLocalObject {
        fn interface_name() -> &'static str {
            "test::local"
        }

        fn get_id(_rpc_version: u64) -> u64 {
            88
        }

        fn binding_metadata() -> &'static [crate::GeneratedMethodBindingDescriptor] {
            &[]
        }
    }

    #[test]
    fn release_local_stub_removes_zero_count_stub_and_notifies_optimistic_zones() {
        let service = Service::new("svc", zone(1));
        let event = Arc::new(RecordingEvent::default());
        service.add_service_event(Arc::downgrade(&(event.clone() as Arc<dyn ServiceEvent>)));

        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(12))));
        service.register_stub(&stub);
        {
            let mut guard = stub.lock().expect("stub mutex poisoned");
            guard.add_ref(InterfacePointerKind::Shared, caller_zone(2));
            guard.add_ref(InterfacePointerKind::Optimistic, caller_zone(3));
        }

        let count = service.release_local_stub(stub, false, caller_zone(2));
        assert_eq!(count, 0);
        assert!(service.get_object(Object::new(12)).is_none());

        let released = event.released.lock().expect("released mutex poisoned");
        assert_eq!(released.len(), 1);
        assert_eq!(released[0].0, Object::new(12));
        assert_eq!(released[0].1.get_address().get_subnet(), 3);
    }

    #[test]
    fn release_params_reports_invalid_version_and_missing_object() {
        let service = Service::new("svc", zone(1));

        let invalid = service.release(ReleaseParams {
            protocol_version: HIGHEST_SUPPORTED_VERSION + 1,
            ..Default::default()
        });
        assert_eq!(invalid.error_code, error_codes::INVALID_VERSION());

        let missing = service.release(ReleaseParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            remote_object_id: RemoteObject::from(
                zone(1).with_object(Object::new(7)).expect("object zone"),
            ),
            caller_zone_id: caller_zone(2),
            options: ReleaseOptions::NORMAL,
            in_back_channel: vec![],
        });
        assert_eq!(missing.error_code, error_codes::OBJECT_NOT_FOUND());
    }

    #[test]
    fn release_params_removes_registered_stub() {
        let service = Service::new("svc", zone(1));
        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(33))));
        service.register_stub(&stub);
        stub.lock()
            .expect("stub mutex poisoned")
            .add_ref(InterfacePointerKind::Shared, caller_zone(2));

        let result = service.release(ReleaseParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            remote_object_id: RemoteObject::from(
                zone(1).with_object(Object::new(33)).expect("object zone"),
            ),
            caller_zone_id: caller_zone(2),
            options: ReleaseOptions::NORMAL,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        assert!(service.get_object(Object::new(33)).is_none());
    }

    #[test]
    fn transport_down_releases_zone_refs_and_notifies_deleted_objects() {
        let service = Service::new("svc", zone(1));
        let event = Arc::new(RecordingEvent::default());
        service.add_service_event(Arc::downgrade(&(event.clone() as Arc<dyn ServiceEvent>)));

        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(21))));
        service.register_stub(&stub);
        {
            let mut guard = stub.lock().expect("stub mutex poisoned");
            guard.add_ref(InterfacePointerKind::Shared, caller_zone(5));
        }

        service.transport_down(caller_zone(5), zone(9));
        assert!(service.get_object(Object::new(21)).is_none());

        let released = event.released.lock().expect("released mutex poisoned");
        assert_eq!(released.len(), 1);
        assert_eq!(released[0].0, Object::new(21));
        assert_eq!(released[0].1.get_address().get_subnet(), 9);
    }

    #[test]
    fn object_released_notifies_listeners_with_embedded_zone() {
        let service = Service::new("svc", zone(1));
        let event = Arc::new(RecordingEvent::default());
        let weak: Weak<dyn ServiceEvent> =
            Arc::downgrade(&(event.clone() as Arc<dyn ServiceEvent>));
        service.add_service_event(weak.clone());

        service.object_released(ObjectReleasedParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            remote_object_id: RemoteObject::from(
                zone(8).with_object(Object::new(77)).expect("object zone"),
            ),
            caller_zone_id: caller_zone(1),
            in_back_channel: vec![],
        });

        let released = event.released.lock().expect("released mutex poisoned");
        assert_eq!(released.len(), 1);
        assert_eq!(released[0].0, Object::new(77));
        assert_eq!(released[0].1.get_address().get_subnet(), 8);
        drop(released);

        service.remove_service_event(&weak);
        service.object_released(ObjectReleasedParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            remote_object_id: RemoteObject::from(
                zone(8).with_object(Object::new(78)).expect("object zone"),
            ),
            caller_zone_id: caller_zone(1),
            in_back_channel: vec![],
        });
        assert_eq!(
            event
                .released
                .lock()
                .expect("released mutex poisoned")
                .len(),
            1
        );
    }

    #[test]
    fn transport_down_params_prunes_dead_listeners_and_cleans_stubs() {
        let service = Service::new("svc", zone(1));
        let event = Arc::new(RecordingEvent::default());
        service.add_service_event(Arc::downgrade(&(event.clone() as Arc<dyn ServiceEvent>)));
        {
            let dead = Arc::new(RecordingEvent::default());
            service.add_service_event(Arc::downgrade(&(dead as Arc<dyn ServiceEvent>)));
        }

        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(55))));
        service.register_stub(&stub);
        stub.lock()
            .expect("stub mutex poisoned")
            .add_ref(InterfacePointerKind::Shared, caller_zone(6));

        service.transport_down_from_params(TransportDownParams {
            protocol_version: LOWEST_SUPPORTED_VERSION,
            destination_zone_id: zone(10).into(),
            caller_zone_id: caller_zone(6),
            in_back_channel: vec![],
        });

        assert!(service.get_object(Object::new(55)).is_none());
        let released = event.released.lock().expect("released mutex poisoned");
        assert_eq!(released.len(), 1);
        assert_eq!(released[0].1.get_address().get_subnet(), 10);
    }

    #[test]
    fn service_config_and_remote_object_result_have_reasonable_defaults() {
        let service = Service::from_config(
            "svc",
            ServiceConfig {
                initial_zone: zone(9),
            },
        );
        assert_eq!(service.zone_id().get_address().get_subnet(), 9);

        let result = RemoteObjectResult::new(error_codes::OK(), RemoteObject::from(zone(9)));
        assert_eq!(result.error_code, error_codes::OK());
        assert_eq!(result.descriptor.get_address().get_subnet(), 9);
        assert_eq!(DUMMY_OBJECT_ID, Object::new(u64::MAX));
    }
}
