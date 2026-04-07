//! Rust counterpart of `c++/rpc/include/rpc/internal/base.h`.
//!
//! The C++ `rpc::base<Implementation, Interfaces...>` type does more than just
//! share code: it is the application-facing seam that keeps generated stub
//! plumbing out of user code while still allowing one object to implement many
//! RPC interfaces. The Rust port needs the same architectural seam even if the
//! implementation mechanics differ from C++ CRTP.

use std::any::Any;
use std::marker::PhantomData;
use std::sync::{Arc, Mutex, Weak};

use crate::internal::bindings_fwd::GeneratedMethodBindingDescriptor;
use crate::internal::casting_interface::{CastingInterface, GeneratedRustInterface};
use crate::internal::marshaller_params::{SendParams, SendResult};
use crate::internal::remote_pointer::CreateLocalProxy;
use crate::internal::service::Service;
use crate::internal::stub::ObjectStub;
use crate::rpc_types::{
    BackChannelEntry, CallerZone, Encoding, InterfaceOrdinal, Method, RemoteObject,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct OpaqueValue;

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct DispatchContext {
    pub protocol_version: u64,
    pub encoding_type: Encoding,
    pub tag: u64,
    pub caller_zone_id: CallerZone,
    pub remote_object_id: RemoteObject,
    pub interface_id: InterfaceOrdinal,
    pub method_id: Method,
    pub in_back_channel: Vec<BackChannelEntry>,
    owner_service_addr: Option<usize>,
}

impl From<&SendParams> for DispatchContext {
    fn from(params: &SendParams) -> Self {
        Self {
            protocol_version: params.protocol_version,
            encoding_type: params.encoding_type,
            tag: params.tag,
            caller_zone_id: params.caller_zone_id.clone(),
            remote_object_id: params.remote_object_id.clone(),
            interface_id: params.interface_id,
            method_id: params.method_id,
            in_back_channel: params.in_back_channel.clone(),
            owner_service_addr: None,
        }
    }
}

impl DispatchContext {
    pub fn with_owner_service_ptr(mut self, owner_service: Option<usize>) -> Self {
        self.owner_service_addr = owner_service;
        self
    }

    pub fn current_service(&self) -> Option<&Service> {
        self.owner_service_addr
            .map(|service| unsafe { &*(service as *const Service) })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct DispatchResult {
    pub error_code: i32,
    pub out_back_channel: Vec<BackChannelEntry>,
}

impl DispatchResult {
    pub fn new(error_code: i32, out_back_channel: Vec<BackChannelEntry>) -> Self {
        Self {
            error_code,
            out_back_channel,
        }
    }
}

#[doc(hidden)]
pub trait RpcObject: CastingInterface {
    #[doc(hidden)]
    fn __rpc_get_stub(&self) -> Option<Arc<Mutex<ObjectStub>>> {
        None
    }

    #[doc(hidden)]
    fn __rpc_set_stub(&self, _stub: Weak<Mutex<ObjectStub>>) {}

    #[doc(hidden)]
    fn __rpc_get_local_interface_view(
        self: Arc<Self>,
        _interface_id: InterfaceOrdinal,
    ) -> Option<Arc<dyn Any + Send + Sync>> {
        None
    }

    #[doc(hidden)]
    fn __rpc_owner_service_ptr(&self) -> Option<usize> {
        get_object_stub(self).and_then(|stub| {
            stub.lock()
                .expect("object stub mutex poisoned")
                .owner_service_addr()
        })
    }

    #[doc(hidden)]
    fn __rpc_get_method_metadata(
        &self,
        _interface_id: InterfaceOrdinal,
    ) -> &'static [GeneratedMethodBindingDescriptor] {
        &[]
    }
}

impl<T: ?Sized> RpcObject for T
where
    T: GeneratedRustInterface,
{
    fn __rpc_get_stub(&self) -> Option<Arc<Mutex<ObjectStub>>> {
        self.__rpc_local_object_stub()
    }
}

#[doc(hidden)]
pub struct LocalInterfaceView<T: ?Sized + Send + Sync + 'static> {
    inner: Arc<T>,
}

impl<T: ?Sized + Send + Sync + 'static> LocalInterfaceView<T> {
    pub fn new(inner: Arc<T>) -> Self {
        Self { inner }
    }

    pub fn as_arc(&self) -> Arc<T> {
        self.inner.clone()
    }
}

#[doc(hidden)]
pub fn get_object_stub<T>(object: &T) -> Option<Arc<Mutex<ObjectStub>>>
where
    T: RpcObject + ?Sized,
{
    object.__rpc_get_stub()
}

#[doc(hidden)]
pub trait LocalObjectAdapter<Impl>: Send + Sync + 'static {
    fn interface_name() -> &'static str {
        ""
    }

    fn get_id(_rpc_version: u64) -> u64 {
        0
    }

    fn binding_metadata() -> &'static [GeneratedMethodBindingDescriptor] {
        &[]
    }

    fn supports_interface(interface_id: InterfaceOrdinal) -> bool;

    fn dispatch(implementation: &Impl, context: &DispatchContext, params: SendParams)
    -> SendResult;

    fn method_metadata(
        _interface_id: InterfaceOrdinal,
    ) -> &'static [GeneratedMethodBindingDescriptor] {
        &[]
    }

    fn local_interface_view(
        _object: Arc<RpcBase<Impl, Self>>,
        _interface_id: InterfaceOrdinal,
    ) -> Option<Arc<dyn Any + Send + Sync>>
    where
        Self: Sized,
    {
        None
    }
}

pub struct RpcBase<Impl, Adapter> {
    implementation: Impl,
    stub: Mutex<Weak<Mutex<ObjectStub>>>,
    _adapter: PhantomData<Adapter>,
}

impl<Impl, Adapter> RpcBase<Impl, Adapter> {
    pub fn new(implementation: Impl) -> Self {
        Self {
            implementation,
            stub: Mutex::new(Weak::new()),
            _adapter: PhantomData,
        }
    }

    pub fn implementation(&self) -> &Impl {
        &self.implementation
    }
}

#[doc(hidden)]
pub fn make_rpc_object_with_adapter<Impl, Adapter>(
    implementation: Impl,
) -> Arc<RpcBase<Impl, Adapter>>
where
    Impl: Send + Sync + 'static,
    Adapter: LocalObjectAdapter<Impl>,
{
    Arc::new(RpcBase::new(implementation))
}

impl<Impl, Adapter> CastingInterface for RpcBase<Impl, Adapter>
where
    Impl: Send + Sync + 'static,
    Adapter: LocalObjectAdapter<Impl>,
{
    fn __rpc_query_interface(&self, interface_id: InterfaceOrdinal) -> bool {
        Adapter::supports_interface(interface_id)
    }

    fn __rpc_call(&self, params: SendParams) -> SendResult {
        let context =
            DispatchContext::from(&params).with_owner_service_ptr(self.__rpc_owner_service_ptr());
        Adapter::dispatch(&self.implementation, &context, params)
    }
}

impl<Impl, Adapter> RpcObject for RpcBase<Impl, Adapter>
where
    Impl: Send + Sync + 'static,
    Adapter: LocalObjectAdapter<Impl>,
{
    fn __rpc_get_stub(&self) -> Option<Arc<Mutex<ObjectStub>>> {
        self.stub
            .lock()
            .expect("rpc base stub mutex poisoned")
            .upgrade()
    }

    fn __rpc_set_stub(&self, stub: Weak<Mutex<ObjectStub>>) {
        *self.stub.lock().expect("rpc base stub mutex poisoned") = stub;
    }

    fn __rpc_get_local_interface_view(
        self: Arc<Self>,
        interface_id: InterfaceOrdinal,
    ) -> Option<Arc<dyn Any + Send + Sync>> {
        Adapter::local_interface_view(self, interface_id)
    }

    fn __rpc_get_method_metadata(
        &self,
        interface_id: InterfaceOrdinal,
    ) -> &'static [GeneratedMethodBindingDescriptor] {
        Adapter::method_metadata(interface_id)
    }
}

impl<Impl, Adapter> CreateLocalProxy for RpcBase<Impl, Adapter>
where
    Impl: Send + Sync + 'static,
    Adapter: LocalObjectAdapter<Impl>,
{
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex, Weak};

    use super::{DispatchContext, LocalObjectAdapter, RpcBase, RpcObject};
    use crate::internal::casting_interface::CastingInterface;
    use crate::internal::marshaller_params::{SendParams, SendResult};
    use crate::internal::remote_pointer::{CreateLocalProxy, LocalProxy};
    use crate::internal::stub::ObjectStub;
    use crate::rpc_types::{InterfaceOrdinal, Method, Object};

    #[derive(Debug)]
    struct Demo {
        value: u8,
    }

    impl CreateLocalProxy for Demo {
        fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
            LocalProxy::new(weak)
        }
    }

    struct DemoAdapter;

    impl LocalObjectAdapter<Demo> for DemoAdapter {
        fn supports_interface(interface_id: InterfaceOrdinal) -> bool {
            interface_id == InterfaceOrdinal::new(7) || interface_id == InterfaceOrdinal::new(9)
        }

        fn dispatch(
            implementation: &Demo,
            context: &DispatchContext,
            params: SendParams,
        ) -> SendResult {
            SendResult::new(
                0,
                vec![
                    implementation.value,
                    context.method_id.get_val() as u8,
                    params.in_back_channel.len() as u8,
                ],
                vec![],
            )
        }
    }

    #[test]
    fn rpc_base_supports_multiple_interfaces_and_dispatch_context() {
        let base = RpcBase::<Demo, DemoAdapter>::new(Demo { value: 5 });

        assert!(base.__rpc_query_interface(InterfaceOrdinal::new(7)));
        assert!(base.__rpc_query_interface(InterfaceOrdinal::new(9)));
        assert!(!base.__rpc_query_interface(InterfaceOrdinal::new(11)));

        let mut params = SendParams {
            interface_id: InterfaceOrdinal::new(9),
            method_id: Method::new(3),
            ..Default::default()
        };
        params.in_back_channel.push(Default::default());

        let result = base.__rpc_call(params);
        assert_eq!(result.error_code, 0);
        assert_eq!(result.out_buf, vec![5, 3, 1]);
    }

    #[test]
    fn rpc_base_tracks_attached_stub_like_cpp_base() {
        let base = RpcBase::<Demo, DemoAdapter>::new(Demo { value: 1 });
        assert!(base.__rpc_get_stub().is_none());

        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(12))));
        base.__rpc_set_stub(Arc::downgrade(&stub));

        let attached = base.__rpc_get_stub().expect("attached stub");
        assert_eq!(
            attached.lock().expect("object stub mutex poisoned").id(),
            Object::new(12)
        );
    }
}
