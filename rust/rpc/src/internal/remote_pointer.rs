//! Rust counterpart of `c++/rpc/include/rpc/internal/remote_pointer.h`.
//!
//! This module owns the RPC-aware pointer wrappers and their hidden local proxy
//! machinery. Generated/app-facing APIs should expose `SharedPtr<T>` and
//! `OptimisticPtr<T>` rather than the internal proxy/control details.

use std::sync::{Arc, Weak};

use crate::internal::bindings_fwd::{
    InterfaceBindResult, InterfaceBindingOrigin, InterfacePointerKind, RemoteObjectBindResult,
};
use crate::internal::casting_interface::CastingInterface;
use crate::internal::object_proxy::ObjectProxy;
use crate::internal::runtime_traits::ServiceRuntime;
use crate::internal::service_proxy::ProxyCaller;
use crate::rpc_types::{AddRefOptions, CallerZone, Object, ReleaseOptions, RemoteObject, Zone};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BoundInterface<T> {
    Null,
    Gone,
    Value(T),
}

impl<T> BoundInterface<T> {
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    pub fn is_gone(&self) -> bool {
        matches!(self, Self::Gone)
    }

    pub fn as_ref(&self) -> Option<&T> {
        match self {
            Self::Value(value) => Some(value),
            Self::Null | Self::Gone => None,
        }
    }
}

#[derive(Debug)]
pub struct Shared<T> {
    pub(crate) iface: BoundInterface<T>,
    remote: Option<Arc<ObjectProxy>>,
}

pub type SharedPtr<T> = Shared<Arc<T>>;

impl<T> Shared<T> {
    pub fn null() -> Self {
        Self {
            iface: BoundInterface::Null,
            remote: None,
        }
    }

    pub fn from_value(value: T) -> Self {
        Self {
            iface: BoundInterface::Value(value),
            remote: None,
        }
    }

    pub fn from_inner(iface: BoundInterface<T>) -> Self {
        Self {
            iface,
            remote: None,
        }
    }

    pub fn into_inner(self) -> BoundInterface<T> {
        let Self { iface, .. } = self;
        iface
    }

    pub fn as_inner(&self) -> &BoundInterface<T> {
        &self.iface
    }

    pub fn as_ref(&self) -> Option<&T> {
        self.iface.as_ref()
    }

    #[doc(hidden)]
    pub fn control_block(&self) -> Option<Arc<ObjectProxy>> {
        self.remote.clone()
    }
}

impl<T> Shared<T>
where
    T: BindableInterfaceValue,
{
    pub fn is_local(&self) -> bool {
        self.remote.is_none() && self.as_ref().is_none_or(BindableInterfaceValue::is_local)
    }

    pub fn remote_object_id(&self) -> Option<RemoteObject> {
        self.remote
            .as_ref()
            .and_then(|remote| remote.remote_object_id())
            .or_else(|| {
                self.as_ref()
                    .and_then(BindableInterfaceValue::remote_object_id)
            })
    }

    pub fn remote_object_proxy(&self) -> Option<Arc<crate::ObjectProxy>> {
        self.remote.as_ref().cloned().or_else(|| {
            self.as_ref()
                .and_then(BindableInterfaceValue::remote_object_proxy)
        })
    }
}

impl<T: ?Sized> Shared<Arc<T>> {
    pub fn from_arc(value: Arc<T>) -> Self {
        Self::from_value(value)
    }

    #[doc(hidden)]
    pub fn from_remote(
        value: Arc<T>,
        _remote_object_id: RemoteObject,
        object_proxy: Arc<ObjectProxy>,
    ) -> Self {
        Self::from_remote_block(value, object_proxy)
    }

    #[doc(hidden)]
    pub fn from_remote_block(value: Arc<T>, remote: Arc<ObjectProxy>) -> Self {
        Self {
            iface: BoundInterface::Value(value),
            remote: Some(remote),
        }
    }

    #[doc(hidden)]
    pub fn adopt_remote_block(value: Arc<T>, remote: Arc<ObjectProxy>) -> Self {
        Self::from_remote_block(value, remote)
    }

    #[doc(hidden)]
    pub fn create_remote_block(
        value: Arc<T>,
        remote: Arc<ObjectProxy>,
        caller_zone_id: CallerZone,
    ) -> Result<Self, i32> {
        let add_ref =
            remote.add_ref_remote_for_caller(caller_zone_id, InterfacePointerKind::Shared);
        if crate::is_critical(add_ref.error_code) {
            return Err(add_ref.error_code);
        }
        Ok(Self::from_remote_block(value, remote))
    }
}

impl<T> crate::internal::CastingInterface for Shared<Arc<T>>
where
    T: crate::internal::CastingInterface + ?Sized,
{
    fn __rpc_query_interface(&self, interface_id: crate::InterfaceOrdinal) -> bool {
        self.as_ref()
            .is_some_and(|value| value.as_ref().__rpc_query_interface(interface_id))
    }

    fn __rpc_call(&self, params: crate::SendParams) -> crate::SendResult {
        match self.as_ref() {
            Some(value) => value.as_ref().__rpc_call(params),
            None => crate::SendResult::new(crate::OBJECT_GONE(), vec![], vec![]),
        }
    }

    fn __rpc_remote_object_id(&self) -> Option<RemoteObject> {
        self.remote_object_id()
    }

    fn __rpc_remote_object_proxy(&self) -> Option<Arc<ObjectProxy>> {
        self.remote_object_proxy()
    }

    fn __rpc_get_method_metadata(
        &self,
        interface_id: crate::InterfaceOrdinal,
    ) -> &'static [crate::GeneratedMethodBindingDescriptor] {
        self.as_ref()
            .map(|value| value.as_ref().__rpc_get_method_metadata(interface_id))
            .unwrap_or(&[])
    }
}

impl<T> Clone for Shared<T>
where
    T: Clone,
{
    fn clone(&self) -> Self {
        Self {
            iface: self.iface.clone(),
            remote: self.remote.clone(),
        }
    }
}

impl<T> PartialEq for Shared<T>
where
    T: PartialEq,
{
    fn eq(&self, other: &Self) -> bool {
        self.iface == other.iface
    }
}

impl<T> Eq for Shared<T> where T: Eq {}

#[derive(Debug)]
pub struct Optimistic<T> {
    pub(crate) iface: BoundInterface<T>,
    remote: Option<Arc<ObjectProxy>>,
}

pub type OptimisticPtr<T> = Optimistic<LocalProxy<T>>;

impl<T> Optimistic<T> {
    pub fn null() -> Self {
        Self {
            iface: BoundInterface::Null,
            remote: None,
        }
    }

    pub fn gone() -> Self {
        Self {
            iface: BoundInterface::Gone,
            remote: None,
        }
    }

    pub fn from_value(value: T) -> Self {
        Self {
            iface: BoundInterface::Value(value),
            remote: None,
        }
    }

    pub fn from_inner(iface: BoundInterface<T>) -> Self {
        Self {
            iface,
            remote: None,
        }
    }

    pub fn into_inner(self) -> BoundInterface<T> {
        let Self { iface, .. } = self;
        iface
    }

    pub fn as_inner(&self) -> &BoundInterface<T> {
        &self.iface
    }

    pub fn as_ref(&self) -> Option<&T> {
        self.iface.as_ref()
    }

    #[doc(hidden)]
    pub fn control_block(&self) -> Option<Arc<ObjectProxy>> {
        self.remote.clone()
    }
}

impl<T> Optimistic<T>
where
    T: BindableInterfaceValue,
{
    pub fn is_local(&self) -> bool {
        self.remote.is_none() && self.as_ref().is_none_or(BindableInterfaceValue::is_local)
    }

    pub fn remote_object_id(&self) -> Option<RemoteObject> {
        self.remote
            .as_ref()
            .and_then(|remote| remote.remote_object_id())
            .or_else(|| {
                self.as_ref()
                    .and_then(BindableInterfaceValue::remote_object_id)
            })
    }

    pub fn remote_object_proxy(&self) -> Option<Arc<crate::ObjectProxy>> {
        self.remote.as_ref().cloned().or_else(|| {
            self.as_ref()
                .and_then(BindableInterfaceValue::remote_object_proxy)
        })
    }
}

impl<T: ?Sized> Optimistic<LocalProxy<T>> {
    pub fn from_shared(value: &Arc<T>) -> Self {
        Self::from_value(LocalProxy::from_shared(value))
    }

    pub fn from_shared_ptr(value: &SharedPtr<T>) -> Self {
        match value.as_ref() {
            Some(iface) => match value.control_block() {
                Some(remote) => Self::from_remote_block(iface.clone(), remote),
                None => Self::from_shared(iface),
            },
            None if value.as_inner().is_gone() => Self::gone(),
            _ => Self::null(),
        }
    }

    #[doc(hidden)]
    pub fn from_remote(
        value: Arc<T>,
        _remote_object_id: RemoteObject,
        object_proxy: Arc<ObjectProxy>,
    ) -> Self {
        Self::from_remote_block(value, object_proxy)
    }

    #[doc(hidden)]
    pub fn from_remote_block(value: Arc<T>, remote: Arc<ObjectProxy>) -> Self {
        Self {
            iface: BoundInterface::Value(LocalProxy::from_remote(value)),
            remote: Some(remote),
        }
    }

    #[doc(hidden)]
    pub fn adopt_remote_block(value: Arc<T>, remote: Arc<ObjectProxy>) -> Self {
        Self::from_remote_block(value, remote)
    }

    #[doc(hidden)]
    pub fn create_remote_block(
        value: Arc<T>,
        remote: Arc<ObjectProxy>,
        caller_zone_id: CallerZone,
    ) -> Result<Self, i32> {
        let add_ref =
            remote.add_ref_remote_for_caller(caller_zone_id, InterfacePointerKind::Optimistic);
        if crate::is_critical(add_ref.error_code) {
            return Err(add_ref.error_code);
        }
        Ok(Self::from_remote_block(value, remote))
    }

    pub fn upgrade(&self) -> Option<Arc<T>> {
        self.as_ref().and_then(LocalProxy::upgrade)
    }

    pub fn expired(&self) -> bool {
        self.as_ref().is_none_or(LocalProxy::expired)
    }

    pub fn is_null(&self) -> bool {
        self.as_ref().is_none_or(LocalProxy::is_null)
    }

    #[doc(hidden)]
    pub fn from_local_proxy(value: LocalProxy<T>) -> Self {
        Self::from_value(value)
    }
}

impl<T> crate::internal::CastingInterface for Optimistic<LocalProxy<T>>
where
    T: crate::internal::CastingInterface + ?Sized,
{
    fn __rpc_query_interface(&self, interface_id: crate::InterfaceOrdinal) -> bool {
        self.upgrade()
            .is_some_and(|value| value.as_ref().__rpc_query_interface(interface_id))
    }

    fn __rpc_call(&self, params: crate::SendParams) -> crate::SendResult {
        match self.upgrade() {
            Some(value) => value.as_ref().__rpc_call(params),
            None => crate::SendResult::new(crate::OBJECT_GONE(), vec![], vec![]),
        }
    }

    fn __rpc_remote_object_id(&self) -> Option<RemoteObject> {
        self.remote_object_id()
    }

    fn __rpc_remote_object_proxy(&self) -> Option<Arc<ObjectProxy>> {
        self.remote_object_proxy()
    }

    fn __rpc_get_method_metadata(
        &self,
        interface_id: crate::InterfaceOrdinal,
    ) -> &'static [crate::GeneratedMethodBindingDescriptor] {
        self.upgrade()
            .map(|value| value.as_ref().__rpc_get_method_metadata(interface_id))
            .unwrap_or(&[])
    }
}

impl<T> Clone for Optimistic<T>
where
    T: Clone,
{
    fn clone(&self) -> Self {
        Self {
            iface: self.iface.clone(),
            remote: self.remote.clone(),
        }
    }
}

impl<T> PartialEq for Optimistic<T>
where
    T: PartialEq,
{
    fn eq(&self, other: &Self) -> bool {
        self.iface == other.iface
    }
}

impl<T> Eq for Optimistic<T> where T: Eq {}

/// Hidden equivalent of C++ `rpc::local_proxy<T>`.
#[derive(Debug)]
pub struct LocalProxy<T: ?Sized> {
    weak: Option<Weak<T>>,
    remote: Option<Arc<T>>,
    was_bound: bool,
}

impl<T: ?Sized> LocalProxy<T> {
    pub fn new(weak: Weak<T>) -> Self {
        Self {
            weak: Some(weak),
            remote: None,
            was_bound: true,
        }
    }

    pub fn null() -> Self {
        Self {
            weak: None,
            remote: None,
            was_bound: false,
        }
    }

    pub fn from_shared(shared: &Arc<T>) -> Self {
        Self::new(Arc::downgrade(shared))
    }

    pub fn from_remote(remote: Arc<T>) -> Self {
        Self {
            weak: None,
            remote: Some(remote),
            was_bound: true,
        }
    }

    pub fn get_weak(&self) -> Option<Weak<T>> {
        self.weak.clone()
    }

    pub fn upgrade(&self) -> Option<Arc<T>> {
        self.remote
            .clone()
            .or_else(|| self.weak.as_ref().and_then(Weak::upgrade))
    }

    pub fn expired(&self) -> bool {
        self.upgrade().is_none()
    }

    pub fn is_null(&self) -> bool {
        !self.was_bound
    }
}

impl<T: ?Sized> Clone for LocalProxy<T> {
    fn clone(&self) -> Self {
        Self {
            weak: self.weak.clone(),
            remote: self.remote.clone(),
            was_bound: self.was_bound,
        }
    }
}

#[doc(hidden)]
pub trait CreateLocalProxy: Sized {
    fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
        LocalProxy::new(weak)
    }
}

#[doc(hidden)]
pub trait CreateRemoteProxy: Sized {
    fn create_remote_proxy(caller: Arc<dyn ProxyCaller>) -> Self;
}

pub trait BindableInterfaceValue {
    fn is_local(&self) -> bool;

    fn remote_object_id(&self) -> Option<RemoteObject> {
        None
    }

    fn remote_object_proxy(&self) -> Option<Arc<crate::ObjectProxy>> {
        None
    }
}

impl<T> BindableInterfaceValue for Arc<T>
where
    T: CastingInterface + ?Sized,
{
    fn is_local(&self) -> bool {
        self.remote_object_id().is_none()
    }

    fn remote_object_id(&self) -> Option<RemoteObject> {
        self.as_ref().__rpc_remote_object_id()
    }

    fn remote_object_proxy(&self) -> Option<Arc<crate::ObjectProxy>> {
        self.as_ref().__rpc_remote_object_proxy()
    }
}

impl<T: ?Sized> BindableInterfaceValue for LocalProxy<T>
where
    T: CastingInterface,
{
    fn is_local(&self) -> bool {
        self.upgrade()
            .is_none_or(|value| value.remote_object_id().is_none())
    }

    fn remote_object_id(&self) -> Option<RemoteObject> {
        self.upgrade()
            .and_then(|value| value.as_ref().__rpc_remote_object_id())
    }

    fn remote_object_proxy(&self) -> Option<Arc<crate::ObjectProxy>> {
        self.upgrade()
            .and_then(|value| value.as_ref().__rpc_remote_object_proxy())
    }
}

pub fn is_bound_pointer_null<T>(iface: &BoundInterface<T>) -> bool {
    iface.is_null()
}

pub fn is_bound_pointer_gone<T>(iface: &BoundInterface<T>) -> bool {
    iface.is_gone()
}

pub fn add_ref_options_for_pointer_kind(pointer_kind: InterfacePointerKind) -> AddRefOptions {
    if pointer_kind.is_optimistic() {
        AddRefOptions::OPTIMISTIC
    } else {
        AddRefOptions::NORMAL
    }
}

pub fn release_options_for_pointer_kind(pointer_kind: InterfacePointerKind) -> ReleaseOptions {
    if pointer_kind.is_optimistic() {
        ReleaseOptions::OPTIMISTIC
    } else {
        ReleaseOptions::NORMAL
    }
}

pub fn null_remote_descriptor() -> RemoteObject {
    RemoteObject::default()
}

pub fn bind_null<T>(error_code: i32) -> InterfaceBindResult<T> {
    InterfaceBindResult::null(error_code)
}

pub fn bind_gone<T>(origin: InterfaceBindingOrigin) -> InterfaceBindResult<T> {
    InterfaceBindResult::gone(crate::internal::error_codes::OBJECT_GONE(), origin)
}

pub fn bind_local_value<T>(iface: T) -> InterfaceBindResult<T> {
    InterfaceBindResult::local(crate::internal::error_codes::OK(), iface)
}

pub fn bind_remote_value<T>(iface: T) -> InterfaceBindResult<T> {
    InterfaceBindResult::remote(crate::internal::error_codes::OK(), iface)
}

pub fn bind_local_optimistic_from_shared<T>(iface: &Arc<T>) -> InterfaceBindResult<OptimisticPtr<T>>
where
    T: ?Sized,
{
    InterfaceBindResult::local(
        crate::internal::error_codes::OK(),
        OptimisticPtr::from_shared(iface),
    )
}

pub fn bind_local_optimistic_from_weak<T>(iface: Weak<T>) -> InterfaceBindResult<OptimisticPtr<T>>
where
    T: ?Sized,
{
    let proxy = LocalProxy::new(iface);
    if proxy.expired() {
        InterfaceBindResult::gone(
            crate::internal::error_codes::OBJECT_GONE(),
            InterfaceBindingOrigin::Local,
        )
    } else {
        InterfaceBindResult::local(
            crate::internal::error_codes::OK(),
            OptimisticPtr::from_value(proxy),
        )
    }
}

pub fn bind_outgoing_interface<T, Stub, BindLocal, BindRemote>(
    iface: &BoundInterface<T>,
    pointer_kind: InterfacePointerKind,
    bind_local: BindLocal,
    bind_remote: BindRemote,
) -> RemoteObjectBindResult<Stub>
where
    T: BindableInterfaceValue,
    BindLocal: FnOnce(&T, InterfacePointerKind) -> RemoteObjectBindResult<Stub>,
    BindRemote: FnOnce(&T, InterfacePointerKind) -> RemoteObjectBindResult<Stub>,
{
    if is_bound_pointer_gone(iface) {
        return RemoteObjectBindResult::new(
            crate::internal::error_codes::OBJECT_GONE(),
            None,
            null_remote_descriptor(),
        );
    }

    let BoundInterface::Value(iface) = iface else {
        return RemoteObjectBindResult::new(
            crate::internal::error_codes::OK(),
            None,
            null_remote_descriptor(),
        );
    };

    if iface.is_local() {
        bind_local(iface, pointer_kind)
    } else {
        bind_remote(iface, pointer_kind)
    }
}

#[doc(hidden)]
pub fn optimistic_from_binding<T: ?Sized>(
    iface: BoundInterface<OptimisticPtr<T>>,
) -> OptimisticPtr<T> {
    match iface {
        BoundInterface::Null => Optimistic::null(),
        BoundInterface::Gone => Optimistic::gone(),
        BoundInterface::Value(value) => value,
    }
}

#[doc(hidden)]
pub fn shared_from_binding<T: ?Sized>(iface: BoundInterface<SharedPtr<T>>) -> SharedPtr<T> {
    match iface {
        BoundInterface::Null => Shared::null(),
        BoundInterface::Gone => Shared::from_inner(BoundInterface::Gone),
        BoundInterface::Value(value) => value,
    }
}

#[doc(hidden)]
pub fn create_remote_shared_interface<T, F>(
    service: &dyn ServiceRuntime,
    remote_object_id: RemoteObject,
    build: F,
) -> Result<SharedPtr<T>, i32>
where
    T: ?Sized,
    F: FnOnce(Arc<dyn ProxyCaller>) -> SharedPtr<T>,
{
    let Some(caller) = service.make_remote_caller(remote_object_id, InterfacePointerKind::Shared)
    else {
        return Err(crate::TRANSPORT_ERROR());
    };
    Ok(build(caller))
}

#[doc(hidden)]
pub fn create_remote_optimistic_interface<T, F>(
    service: &dyn ServiceRuntime,
    remote_object_id: RemoteObject,
    build: F,
) -> Result<OptimisticPtr<T>, i32>
where
    T: ?Sized,
    F: FnOnce(Arc<dyn ProxyCaller>) -> OptimisticPtr<T>,
{
    let Some(caller) =
        service.make_remote_caller(remote_object_id, InterfacePointerKind::Optimistic)
    else {
        return Err(crate::TRANSPORT_ERROR());
    };
    Ok(build(caller))
}

pub fn bind_incoming_shared<T, LocalLookup, RemoteBind>(
    service_zone: &Zone,
    encap: &RemoteObject,
    lookup_local: LocalLookup,
    bind_remote: RemoteBind,
) -> InterfaceBindResult<T>
where
    LocalLookup: FnOnce(Object) -> Result<T, i32>,
    RemoteBind: FnOnce(&RemoteObject) -> InterfaceBindResult<T>,
{
    if *encap == null_remote_descriptor() || !encap.is_set() {
        return bind_null(crate::internal::error_codes::OK());
    }

    if encap.as_zone() == *service_zone {
        return match lookup_local(encap.get_object_id()) {
            Ok(iface) => bind_local_value(iface),
            Err(error_code) => bind_null(error_code),
        };
    }

    bind_remote(encap)
}

pub fn bind_incoming_optimistic<T, LocalLookup, RemoteBind>(
    service_zone: &Zone,
    encap: &RemoteObject,
    lookup_local: LocalLookup,
    bind_remote: RemoteBind,
) -> InterfaceBindResult<OptimisticPtr<T>>
where
    T: ?Sized,
    LocalLookup: FnOnce(Object) -> Result<Arc<T>, i32>,
    RemoteBind: FnOnce(&RemoteObject) -> InterfaceBindResult<OptimisticPtr<T>>,
{
    if *encap == null_remote_descriptor() || !encap.is_set() {
        return bind_null(crate::internal::error_codes::OK());
    }

    if encap.as_zone() == *service_zone {
        return match lookup_local(encap.get_object_id()) {
            Ok(iface) => bind_local_optimistic_from_shared(&iface),
            Err(error_code) if error_code == crate::internal::error_codes::OBJECT_GONE() => {
                bind_gone(InterfaceBindingOrigin::Local)
            }
            Err(error_code) => bind_null(error_code),
        };
    }

    bind_remote(encap)
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{
        BindableInterfaceValue, BoundInterface, CreateLocalProxy, Optimistic, OptimisticPtr,
        Shared, SharedPtr, add_ref_options_for_pointer_kind, bind_gone, bind_incoming_optimistic,
        bind_incoming_shared, bind_local_optimistic_from_shared, bind_local_optimistic_from_weak,
        bind_local_value, bind_null, bind_outgoing_interface, bind_remote_value,
        is_bound_pointer_gone, is_bound_pointer_null, release_options_for_pointer_kind,
    };
    use crate::internal::RemoteObjectBindResult;
    use crate::internal::bindings_fwd::{InterfaceBindingOrigin, InterfacePointerKind};
    use crate::internal::error_codes;
    use crate::rpc_types::{
        AddRefOptions, Object, ReleaseOptions, RemoteObject, Zone, ZoneAddress, ZoneAddressArgs,
    };

    #[derive(Debug)]
    struct Example;

    impl CreateLocalProxy for Example {}

    impl crate::internal::casting_interface::CastingInterface for Example {}

    #[derive(Debug, Clone, PartialEq, Eq)]
    struct FakeIface {
        local: bool,
        label: &'static str,
    }

    impl BindableInterfaceValue for FakeIface {
        fn is_local(&self) -> bool {
            self.local
        }
    }

    fn zone(subnet: u64) -> Zone {
        let mut args = ZoneAddressArgs::default();
        args.subnet = subnet;
        Zone::new(ZoneAddress::create(args).expect("zone address"))
    }

    fn remote_object(subnet: u64, object_id: u64) -> RemoteObject {
        zone(subnet)
            .with_object(Object::new(object_id))
            .expect("remote object")
    }

    #[test]
    fn optimistic_bound_interface_can_be_gone_without_being_null() {
        let gone = BoundInterface::<u64>::Gone;
        assert!(is_bound_pointer_gone(&gone));
        assert!(!is_bound_pointer_null(&gone));
    }

    #[test]
    fn pointer_kind_maps_to_refcount_flags() {
        assert_eq!(
            add_ref_options_for_pointer_kind(InterfacePointerKind::Shared),
            AddRefOptions::NORMAL
        );
        assert_eq!(
            add_ref_options_for_pointer_kind(InterfacePointerKind::Optimistic),
            AddRefOptions::OPTIMISTIC
        );
        assert_eq!(
            release_options_for_pointer_kind(InterfacePointerKind::Shared),
            ReleaseOptions::NORMAL
        );
        assert_eq!(
            release_options_for_pointer_kind(InterfacePointerKind::Optimistic),
            ReleaseOptions::OPTIMISTIC
        );
    }

    #[test]
    fn local_and_remote_value_bind_helpers_preserve_origin() {
        let local = bind_local_value(7_u32);
        let remote = bind_remote_value(9_u32);

        assert!(local.is_local());
        assert_eq!(local.iface, BoundInterface::Value(7));
        assert!(remote.is_remote());
        assert_eq!(remote.iface, BoundInterface::Value(9));
    }

    #[test]
    fn local_optimistic_binding_from_live_shared_uses_local_proxy() {
        let iface = Arc::new(Example);
        let result = bind_local_optimistic_from_shared(&iface);

        assert!(result.is_local());
        let BoundInterface::Value(proxy) = result.iface else {
            panic!("expected live local proxy");
        };
        let Some(proxy) = proxy.as_ref() else {
            panic!("expected live local proxy");
        };
        assert!(!proxy.expired());
        assert!(proxy.upgrade().is_some());
    }

    #[test]
    fn local_optimistic_binding_from_expired_weak_is_gone() {
        let iface = Arc::new(Example);
        let weak = Arc::downgrade(&iface);
        drop(iface);

        let result = bind_local_optimistic_from_weak::<Example>(weak);

        assert_eq!(result.error_code, error_codes::OBJECT_GONE());
        assert!(result.is_local());
        assert!(matches!(
            result.iface,
            BoundInterface::<OptimisticPtr<Example>>::Gone
        ));
    }

    #[test]
    fn explicit_null_and_gone_bind_helpers_match_cpp_shape() {
        let null = bind_null::<u32>(error_codes::OK());
        let gone = bind_gone::<u32>(InterfaceBindingOrigin::Remote);

        assert_eq!(null.iface, BoundInterface::Null);
        assert_eq!(gone.iface, BoundInterface::Gone);
        assert!(gone.is_remote());
    }

    #[test]
    fn shared_and_optimistic_wrappers_preserve_bound_states() {
        let shared = Shared::from_value(7_u32);
        let optimistic = Optimistic::<u32>::gone();

        assert_eq!(shared.into_inner(), BoundInterface::Value(7));
        assert_eq!(optimistic.into_inner(), BoundInterface::Gone);
    }

    #[test]
    fn app_facing_pointer_aliases_accept_dyn_interface_targets() {
        let shared =
            SharedPtr::<dyn crate::internal::casting_interface::CastingInterface>::from_arc(
                Arc::new(Example),
            );
        assert!(shared.as_ref().is_some());

        let iface: Arc<dyn crate::internal::casting_interface::CastingInterface> =
            Arc::new(Example);
        let optimistic =
            OptimisticPtr::<dyn crate::internal::casting_interface::CastingInterface>::from_shared(
                &iface,
            );
        let Some(proxy) = optimistic.as_ref() else {
            panic!("expected optimistic proxy");
        };
        assert!(proxy.upgrade().is_some());
    }

    #[test]
    fn optimistic_from_remote_shared_ptr_reuses_hidden_control_block() {
        let remote_iface: Arc<dyn crate::internal::casting_interface::CastingInterface> =
            Arc::new(Example);
        let remote_object = remote_object(9, 44);
        let object_proxy = Arc::new(crate::ObjectProxy::new_remote_identity(
            remote_object.clone(),
        ));
        let shared = SharedPtr::from_remote(
            remote_iface.clone(),
            remote_object.clone(),
            object_proxy.clone(),
        );

        let optimistic = OptimisticPtr::from_shared_ptr(&shared);

        assert_eq!(optimistic.remote_object_id(), Some(remote_object));
        assert!(Arc::ptr_eq(
            &optimistic.remote_object_proxy().expect("remote proxy"),
            &object_proxy,
        ));
        assert!(Arc::ptr_eq(
            &shared.control_block().expect("shared control block"),
            &optimistic
                .control_block()
                .expect("optimistic control block"),
        ));
        let upgraded = optimistic
            .upgrade()
            .expect("optimistic remote interface should upgrade");
        assert!(Arc::ptr_eq(&upgraded, &remote_iface));
    }

    #[test]
    fn incoming_shared_binding_uses_local_lookup_for_same_zone() {
        let result = bind_incoming_shared(
            &zone(7),
            &remote_object(7, 99),
            |object_id| {
                assert_eq!(object_id, Object::new(99));
                Ok(FakeIface {
                    local: true,
                    label: "local",
                })
            },
            |_| {
                panic!("remote bind should not run");
            },
        );

        assert!(result.is_local());
        assert_eq!(
            result.iface,
            BoundInterface::Value(FakeIface {
                local: true,
                label: "local",
            })
        );
    }

    #[test]
    fn incoming_shared_binding_uses_remote_path_for_foreign_zone() {
        let result = bind_incoming_shared(
            &zone(7),
            &remote_object(8, 55),
            |_| {
                panic!("local lookup should not run");
            },
            |encap| {
                assert_eq!(encap.get_object_id(), Object::new(55));
                bind_remote_value(FakeIface {
                    local: false,
                    label: "remote",
                })
            },
        );

        assert!(result.is_remote());
        assert_eq!(
            result.iface,
            BoundInterface::Value(FakeIface {
                local: false,
                label: "remote",
            })
        );
    }

    #[test]
    fn incoming_optimistic_binding_uses_local_proxy_for_same_zone() {
        let iface = Arc::new(Example);
        let keeper = iface.clone();
        let result = bind_incoming_optimistic(
            &zone(3),
            &remote_object(3, 12),
            move |_| Ok(iface),
            |_| panic!("remote bind should not run"),
        );

        assert!(result.is_local());
        let BoundInterface::Value(proxy) = result.iface else {
            panic!("expected local optimistic proxy");
        };
        let Some(proxy) = proxy.as_ref() else {
            panic!("expected local optimistic proxy value");
        };
        assert_eq!(Arc::strong_count(&keeper), 1);
        assert!(!proxy.expired());
    }

    #[test]
    fn outgoing_binding_routes_by_locality_and_pointer_kind() {
        let local_iface = BoundInterface::Value(FakeIface {
            local: true,
            label: "local",
        });
        let remote_iface = BoundInterface::Value(FakeIface {
            local: false,
            label: "remote",
        });

        let local_result = bind_outgoing_interface(
            &local_iface,
            InterfacePointerKind::Optimistic,
            |iface, pointer_kind| {
                assert!(iface.is_local());
                assert_eq!(pointer_kind, InterfacePointerKind::Optimistic);
                RemoteObjectBindResult::new(error_codes::OK(), Some("stub"), remote_object(1, 1))
            },
            |_, _| {
                panic!("remote binder should not run");
            },
        );

        let remote_result = bind_outgoing_interface(
            &remote_iface,
            InterfacePointerKind::Shared,
            |_, _| {
                panic!("local binder should not run");
            },
            |iface, pointer_kind| {
                assert!(!iface.is_local());
                assert_eq!(pointer_kind, InterfacePointerKind::Shared);
                RemoteObjectBindResult::new(error_codes::OK(), Some("proxy"), remote_object(2, 2))
            },
        );

        assert_eq!(local_result.stub, Some("stub"));
        assert_eq!(remote_result.stub, Some("proxy"));
    }

    #[test]
    fn outgoing_binding_preserves_null_and_gone() {
        let null_result: RemoteObjectBindResult<&'static str> = bind_outgoing_interface(
            &BoundInterface::<FakeIface>::Null,
            InterfacePointerKind::Shared,
            |_, _| panic!("binder should not run"),
            |_, _| panic!("binder should not run"),
        );

        let gone_result: RemoteObjectBindResult<&'static str> = bind_outgoing_interface(
            &BoundInterface::<FakeIface>::Gone,
            InterfacePointerKind::Optimistic,
            |_, _| panic!("binder should not run"),
            |_, _| panic!("binder should not run"),
        );

        assert_eq!(null_result.error_code, crate::internal::error_codes::OK());
        assert_eq!(null_result.descriptor, RemoteObject::default());
        assert_eq!(
            gone_result.error_code,
            crate::internal::error_codes::OBJECT_GONE()
        );
        assert_eq!(gone_result.descriptor, RemoteObject::default());
    }
}
