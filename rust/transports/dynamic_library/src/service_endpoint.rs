//! Service-backed child endpoint for the dynamic-library C ABI.
//!
//! This is the Rust DLL-side counterpart to the host-side dynamic-library
//! transport: inbound C ABI calls are decoded by `ChildTransportAdapter` and
//! forwarded through this endpoint into the Rust `Service`.

use crate::ffi::{CanopyDllInitParams, ParentCallbacks, copy_zone};
use canopy_rpc::{
    AddRefParams, GetNewZoneIdParams, IMarshaller, NewZoneIdResult, ObjectReleasedParams,
    PostParams, ReleaseParams, SendParams, SendResult, ServiceRuntime, StandardResult, Transport,
    TransportDownParams, TransportStatus, TryCastParams,
};
use std::sync::{Arc, Weak};

#[derive(Clone)]
struct ParentCallbackMarshaller(ParentCallbacks);

// The parent callback table is a stable C ABI handle owned by the C++ parent
// side for the lifetime of the child context. The Rust Transport requires a
// Send + Sync outbound marshaller, matching the C++ transport ownership model.
unsafe impl Send for ParentCallbackMarshaller {}
unsafe impl Sync for ParentCallbackMarshaller {}

impl IMarshaller for ParentCallbackMarshaller {
    fn send(&self, params: SendParams) -> SendResult {
        self.0.send(params)
    }

    fn post(&self, params: PostParams) {
        self.0.post(params);
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.0.try_cast(params)
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.0.add_ref(params)
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        self.0.release(params)
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        self.0.object_released(params);
    }

    fn transport_down(&self, params: TransportDownParams) {
        self.0.transport_down(params);
    }

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        self.0.get_new_zone_id(params)
    }
}

#[derive(Clone)]
pub struct ChildServiceEndpoint {
    service: Arc<dyn ServiceRuntime>,
    parent_transport: Option<Arc<Transport>>,
}

impl ChildServiceEndpoint {
    pub fn new(service: Arc<impl ServiceRuntime + 'static>) -> Self {
        Self {
            service,
            parent_transport: None,
        }
    }

    pub fn with_parent_transport(
        service: Arc<impl ServiceRuntime + 'static>,
        parent_transport: Arc<Transport>,
    ) -> Self {
        Self {
            service,
            parent_transport: Some(parent_transport),
        }
    }

    pub fn service(&self) -> &Arc<dyn ServiceRuntime> {
        &self.service
    }

    pub fn parent_transport(&self) -> Option<&Arc<Transport>> {
        self.parent_transport.as_ref()
    }
}

impl IMarshaller for ChildServiceEndpoint {
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
        self.service.get_new_zone_id(params)
    }
}

fn weak_service<S>(service: &Arc<S>) -> Weak<dyn ServiceRuntime>
where
    S: ServiceRuntime + 'static,
{
    let service_runtime: Arc<dyn ServiceRuntime> = service.clone();
    Arc::downgrade(&service_runtime)
}

pub fn register_parent_transport_from_init_params(
    name: impl Into<String>,
    service: &Arc<impl ServiceRuntime + 'static>,
    params: &CanopyDllInitParams,
) -> Arc<Transport> {
    let child_zone = copy_zone(params.child_zone);
    let parent_zone = copy_zone(params.parent_zone);
    let parent_callbacks = ParentCallbackMarshaller(ParentCallbacks::from_init_params(params));
    let parent_transport = Transport::new(
        name,
        child_zone,
        parent_zone.clone(),
        weak_service(service),
        Arc::new(parent_callbacks),
    );
    parent_transport.set_status(TransportStatus::Connected);
    service.add_transport(parent_zone, parent_transport.clone());
    parent_transport
}

pub fn create_child_zone_from_init_params(
    name: impl Into<String>,
    service: Arc<impl ServiceRuntime + 'static>,
    params: &CanopyDllInitParams,
) -> ChildServiceEndpoint {
    let parent_transport = register_parent_transport_from_init_params(name, &service, params);
    ChildServiceEndpoint::with_parent_transport(service, parent_transport)
}

pub fn create_child_zone_with_exported_object_from_init_params(
    name: impl Into<String>,
    service: Arc<impl ServiceRuntime + 'static>,
    params: &CanopyDllInitParams,
    object_id: canopy_rpc::Object,
    exported_object: Arc<dyn canopy_rpc::RpcObject>,
) -> Result<(ChildServiceEndpoint, canopy_rpc::RemoteObject), i32> {
    let endpoint = create_child_zone_from_init_params(name, service.clone(), params);
    service
        .register_rpc_object(object_id, exported_object)
        .map_err(|error_code| error_code)?;
    let remote_object = service
        .zone_id()
        .with_object(object_id)
        .map_err(|_| canopy_rpc::INVALID_DATA())?;
    Ok((endpoint, remote_object))
}
