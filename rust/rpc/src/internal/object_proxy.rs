//! Rust counterpart of `c++/rpc/include/rpc/internal/object_proxy.h`.
//!
//! The key architectural rule to preserve from C++ is:
//! - one `ObjectProxy` owns one remote object identity
//! - multiple interface-specific proxy views can hang off that same object
//! - a cast returns a different interface view backed by the same `ObjectProxy`

use std::any::Any;
use std::collections::{BTreeMap, HashMap};
use std::marker::PhantomData;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex, Weak};

use crate::internal::bindings_fwd::InterfacePointerKind;
use crate::internal::error_codes;
use crate::internal::service_proxy::ServiceProxy;
use crate::rpc_types::{CallerZone, InterfaceOrdinal, Object, ReleaseOptions, RemoteObject};

/// Rust equivalent of C++ `query_interface_result`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QueryInterfaceResult<Ptr> {
    pub error_code: i32,
    pub iface: Option<Ptr>,
}

impl<Ptr> QueryInterfaceResult<Ptr> {
    pub fn new(error_code: i32, iface: Option<Ptr>) -> Self {
        Self { error_code, iface }
    }
}

/// Interface-specific remote proxy view backed by one shared `ObjectProxy`.
#[derive(Debug)]
pub struct RemoteInterfaceView<T> {
    object_proxy: Arc<ObjectProxy>,
    interface_id: InterfaceOrdinal,
    _marker: PhantomData<fn() -> T>,
}

impl<T> RemoteInterfaceView<T> {
    pub fn new(object_proxy: Arc<ObjectProxy>, interface_id: InterfaceOrdinal) -> Self {
        Self {
            object_proxy,
            interface_id,
            _marker: PhantomData,
        }
    }

    pub fn object_proxy(&self) -> Arc<ObjectProxy> {
        self.object_proxy.clone()
    }

    pub fn object_id(&self) -> Object {
        self.object_proxy.object_id()
    }

    pub fn interface_id(&self) -> InterfaceOrdinal {
        self.interface_id
    }
}

impl<T> Clone for RemoteInterfaceView<T> {
    fn clone(&self) -> Self {
        Self {
            object_proxy: self.object_proxy.clone(),
            interface_id: self.interface_id,
            _marker: PhantomData,
        }
    }
}

type ErasedInterfaceView = dyn Any + Send + Sync;

pub struct ObjectProxy {
    object_id: Object,
    remote_object_id: Option<RemoteObject>,
    service_proxy: Option<Arc<ServiceProxy>>,
    shared_count: AtomicU32,
    optimistic_count: AtomicU32,
    shared_caller_counts: Mutex<BTreeMap<CallerZone, u32>>,
    optimistic_caller_counts: Mutex<BTreeMap<CallerZone, u32>>,
    interface_views: Mutex<HashMap<InterfaceOrdinal, Weak<ErasedInterfaceView>>>,
}

pub struct RemoteRefGuard {
    object_proxy: Arc<ObjectProxy>,
    caller_zone_id: CallerZone,
    pointer_kind: InterfacePointerKind,
    active: bool,
}

impl RemoteRefGuard {
    pub fn adopt(
        object_proxy: Arc<ObjectProxy>,
        caller_zone_id: CallerZone,
        pointer_kind: InterfacePointerKind,
    ) -> Option<Self> {
        let result = object_proxy.adopt_remote_ref_for_caller(caller_zone_id.clone(), pointer_kind);
        if crate::is_error(result.error_code) {
            return None;
        }
        Some(Self {
            object_proxy,
            caller_zone_id,
            pointer_kind,
            active: true,
        })
    }
}

impl Drop for RemoteRefGuard {
    fn drop(&mut self) {
        if !self.active {
            return;
        }
        let options = if self.pointer_kind.is_optimistic() {
            ReleaseOptions::OPTIMISTIC
        } else {
            ReleaseOptions::NORMAL
        };
        let _ = self
            .object_proxy
            .release_remote_for_caller(self.caller_zone_id.clone(), options);
    }
}

impl std::fmt::Debug for RemoteRefGuard {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RemoteRefGuard")
            .field("caller_zone_id", &self.caller_zone_id)
            .field("pointer_kind", &self.pointer_kind)
            .field("active", &self.active)
            .finish_non_exhaustive()
    }
}

impl std::fmt::Debug for ObjectProxy {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ObjectProxy")
            .field("object_id", &self.object_id)
            .field("remote_object_id", &self.remote_object_id)
            .field("shared_count", &self.shared_count())
            .field("optimistic_count", &self.optimistic_count())
            .field("interface_view_count", &self.interface_view_count())
            .finish()
    }
}

impl ObjectProxy {
    pub fn new(object_id: Object) -> Self {
        Self {
            object_id,
            remote_object_id: None,
            service_proxy: None,
            shared_count: AtomicU32::new(0),
            optimistic_count: AtomicU32::new(0),
            shared_caller_counts: Mutex::new(BTreeMap::new()),
            optimistic_caller_counts: Mutex::new(BTreeMap::new()),
            interface_views: Mutex::new(HashMap::new()),
        }
    }

    pub fn new_remote(remote_object_id: RemoteObject, service_proxy: &Arc<ServiceProxy>) -> Self {
        Self::new_remote_with_service_proxy(remote_object_id, service_proxy.clone())
    }

    pub fn new_remote_with_service_proxy(
        remote_object_id: RemoteObject,
        service_proxy: Arc<ServiceProxy>,
    ) -> Self {
        Self {
            object_id: remote_object_id.get_object_id(),
            remote_object_id: Some(remote_object_id),
            service_proxy: Some(service_proxy),
            shared_count: AtomicU32::new(0),
            optimistic_count: AtomicU32::new(0),
            shared_caller_counts: Mutex::new(BTreeMap::new()),
            optimistic_caller_counts: Mutex::new(BTreeMap::new()),
            interface_views: Mutex::new(HashMap::new()),
        }
    }

    pub fn object_id(&self) -> Object {
        self.object_id
    }

    pub fn add_ref_remote_for_caller(
        &self,
        caller_zone_id: CallerZone,
        pointer_kind: InterfacePointerKind,
    ) -> crate::StandardResult {
        let prev_count = self.increment_remote_ref_count(caller_zone_id.clone(), pointer_kind);
        if prev_count != 0 {
            return crate::StandardResult::new(crate::OK(), vec![]);
        }

        let Some(service_proxy) = self.service_proxy() else {
            self.decrement_remote_ref_count(caller_zone_id, pointer_kind);
            return crate::StandardResult::new(crate::TRANSPORT_ERROR(), vec![]);
        };
        let result = service_proxy.add_ref_remote_object_for_caller(
            self.object_id,
            caller_zone_id.clone(),
            pointer_kind,
        );
        if error_codes::is_error(result.error_code) {
            self.decrement_remote_ref_count(caller_zone_id, pointer_kind);
        }
        result
    }

    pub fn adopt_remote_ref_for_caller(
        &self,
        caller_zone_id: CallerZone,
        pointer_kind: InterfacePointerKind,
    ) -> crate::StandardResult {
        self.increment_remote_ref_count(caller_zone_id, pointer_kind);
        crate::StandardResult::new(crate::OK(), vec![])
    }

    pub fn release_remote_for_caller(
        &self,
        caller_zone_id: CallerZone,
        options: ReleaseOptions,
    ) -> crate::StandardResult {
        let pointer_kind = if options == ReleaseOptions::OPTIMISTIC {
            InterfacePointerKind::Optimistic
        } else {
            InterfacePointerKind::Shared
        };
        let prev_count = self.decrement_remote_ref_count(caller_zone_id.clone(), pointer_kind);

        if prev_count != 1 {
            return crate::StandardResult::new(crate::OK(), vec![]);
        }

        let Some(service_proxy) = self.service_proxy() else {
            return crate::StandardResult::new(crate::TRANSPORT_ERROR(), vec![]);
        };
        service_proxy.release_remote_object_for_caller(self.object_id, caller_zone_id, options)
    }

    fn increment_remote_ref_count(
        &self,
        caller_zone_id: CallerZone,
        pointer_kind: InterfacePointerKind,
    ) -> u32 {
        let (global_count, caller_counts) = match pointer_kind {
            InterfacePointerKind::Shared => (&self.shared_count, &self.shared_caller_counts),
            InterfacePointerKind::Optimistic => {
                (&self.optimistic_count, &self.optimistic_caller_counts)
            }
        };
        let mut counts = caller_counts
            .lock()
            .expect("object proxy caller count mutex poisoned");
        let count = counts.entry(caller_zone_id).or_insert(0);
        let previous = *count;
        *count = count.saturating_add(1);
        global_count.fetch_add(1, Ordering::Relaxed);
        previous
    }

    fn decrement_remote_ref_count(
        &self,
        caller_zone_id: CallerZone,
        pointer_kind: InterfacePointerKind,
    ) -> u32 {
        let (global_count, caller_counts) = match pointer_kind {
            InterfacePointerKind::Shared => (&self.shared_count, &self.shared_caller_counts),
            InterfacePointerKind::Optimistic => {
                (&self.optimistic_count, &self.optimistic_caller_counts)
            }
        };
        let mut counts = caller_counts
            .lock()
            .expect("object proxy caller count mutex poisoned");
        let Some(count) = counts.get_mut(&caller_zone_id) else {
            return 0;
        };
        let previous = *count;
        *count = count.saturating_sub(1);
        global_count
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {
                Some(count.saturating_sub(1))
            })
            .ok();
        if *count == 0 {
            counts.remove(&caller_zone_id);
        }
        previous
    }

    pub fn remote_object_id(&self) -> Option<RemoteObject> {
        self.remote_object_id.clone()
    }

    pub fn service_proxy(&self) -> Option<Arc<ServiceProxy>> {
        self.service_proxy.clone()
    }

    pub(crate) fn service_proxy_ref(&self) -> Option<&ServiceProxy> {
        self.service_proxy.as_deref()
    }

    pub fn shared_count(&self) -> u32 {
        self.shared_count.load(Ordering::Acquire)
    }

    pub fn optimistic_count(&self) -> u32 {
        self.optimistic_count.load(Ordering::Acquire)
    }

    pub fn interface_view_count(&self) -> usize {
        self.interface_views
            .lock()
            .expect("object proxy interface view mutex poisoned")
            .len()
    }

    pub fn add_ref(&self, pointer_kind: InterfacePointerKind) {
        match pointer_kind {
            InterfacePointerKind::Shared => {
                self.shared_count.fetch_add(1, Ordering::Relaxed);
            }
            InterfacePointerKind::Optimistic => {
                self.optimistic_count.fetch_add(1, Ordering::Relaxed);
            }
        }
    }

    pub fn release(&self, options: ReleaseOptions) -> bool {
        if options == ReleaseOptions::OPTIMISTIC {
            self.optimistic_count
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {
                    Some(count.saturating_sub(1))
                })
                .ok();
        } else {
            self.shared_count
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {
                    Some(count.saturating_sub(1))
                })
                .ok();
        }

        self.shared_count() == 0 && self.optimistic_count() == 0
    }

    pub fn add_ref_shared(&self) {
        self.shared_count.fetch_add(1, Ordering::Relaxed);
    }

    pub fn add_ref_optimistic(&self) {
        self.optimistic_count.fetch_add(1, Ordering::Relaxed);
    }

    pub fn get_or_insert_interface_view<T, CreateFn>(
        self: &Arc<Self>,
        interface_id: InterfaceOrdinal,
        create: CreateFn,
    ) -> Arc<T>
    where
        T: Any + Send + Sync + 'static,
        CreateFn: FnOnce(&Arc<Self>) -> Arc<T>,
    {
        let mut guard = self
            .interface_views
            .lock()
            .expect("object proxy interface view mutex poisoned");

        if let Some(existing) = guard.get(&interface_id).and_then(Weak::upgrade) {
            if let Ok(existing_typed) = Arc::downcast::<T>(existing) {
                return existing_typed;
            }
        }

        let typed = create(self);
        let erased: Arc<ErasedInterfaceView> = typed.clone();
        guard.insert(interface_id, Arc::downgrade(&erased));
        typed
    }

    pub fn make_interface_view<T>(
        self: &Arc<Self>,
        interface_id: InterfaceOrdinal,
    ) -> Arc<RemoteInterfaceView<T>>
    where
        T: Send + Sync + 'static,
    {
        self.get_or_insert_interface_view(interface_id, |object_proxy| {
            Arc::new(RemoteInterfaceView::<T>::new(
                object_proxy.clone(),
                interface_id,
            ))
        })
    }

    pub fn query_interface_view<T, TryCastFn>(
        self: &Arc<Self>,
        interface_id: InterfaceOrdinal,
        do_remote_check: bool,
        try_cast: TryCastFn,
    ) -> QueryInterfaceResult<Arc<RemoteInterfaceView<T>>>
    where
        T: Send + Sync + 'static,
        TryCastFn: FnOnce(InterfaceOrdinal) -> i32,
    {
        if !interface_id.is_set() {
            return QueryInterfaceResult::new(error_codes::OK(), None);
        }

        if let Some(existing) = self.lookup_interface_view::<T>(interface_id) {
            return QueryInterfaceResult::new(error_codes::OK(), Some(existing));
        }

        if do_remote_check {
            let ret = try_cast(interface_id);
            if error_codes::is_error(ret) {
                return QueryInterfaceResult::new(ret, None);
            }
        }

        QueryInterfaceResult::new(
            error_codes::OK(),
            Some(self.make_interface_view::<T>(interface_id)),
        )
    }

    pub fn lookup_interface_view<T>(
        self: &Arc<Self>,
        interface_id: InterfaceOrdinal,
    ) -> Option<Arc<RemoteInterfaceView<T>>>
    where
        T: Send + Sync + 'static,
    {
        let guard = self
            .interface_views
            .lock()
            .expect("object proxy interface view mutex poisoned");
        let existing = guard.get(&interface_id)?.upgrade()?;
        Arc::downcast::<RemoteInterfaceView<T>>(existing).ok()
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{ObjectProxy, QueryInterfaceResult, RemoteInterfaceView};
    use crate::internal::bindings_fwd::InterfacePointerKind;
    use crate::internal::error_codes;
    use crate::rpc_types::{InterfaceOrdinal, Object, ReleaseOptions};

    struct FooIface;
    struct BarIface;

    #[test]
    fn object_proxy_tracks_shared_and_optimistic_counts_separately() {
        let proxy = ObjectProxy::new(Object::new(42));
        proxy.add_ref(InterfacePointerKind::Shared);
        proxy.add_ref(InterfacePointerKind::Optimistic);
        proxy.add_ref(InterfacePointerKind::Optimistic);

        assert_eq!(proxy.shared_count(), 1);
        assert_eq!(proxy.optimistic_count(), 2);

        assert!(!proxy.release(ReleaseOptions::OPTIMISTIC));
        assert_eq!(proxy.optimistic_count(), 1);
        assert!(!proxy.release(ReleaseOptions::NORMAL));
        assert_eq!(proxy.shared_count(), 0);
        assert!(proxy.release(ReleaseOptions::OPTIMISTIC));
    }

    #[test]
    fn object_proxy_reuses_same_interface_view_for_same_interface() {
        let proxy = Arc::new(ObjectProxy::new(Object::new(7)));
        let interface_id = InterfaceOrdinal::new(11);

        let first = proxy.make_interface_view::<FooIface>(interface_id);
        let second = proxy.make_interface_view::<FooIface>(interface_id);

        assert!(Arc::ptr_eq(&first, &second));
        assert_eq!(proxy.interface_view_count(), 1);
        assert!(Arc::ptr_eq(&first.object_proxy(), &proxy));
    }

    #[test]
    fn object_proxy_can_hold_multiple_interface_views_for_one_remote_identity() {
        let proxy = Arc::new(ObjectProxy::new(Object::new(8)));

        let foo = proxy.make_interface_view::<FooIface>(InterfaceOrdinal::new(1));
        let bar = proxy.make_interface_view::<BarIface>(InterfaceOrdinal::new(2));

        assert_eq!(foo.object_id(), Object::new(8));
        assert_eq!(bar.object_id(), Object::new(8));
        assert_eq!(proxy.interface_view_count(), 2);
        assert!(Arc::ptr_eq(&foo.object_proxy(), &bar.object_proxy()));
    }

    #[test]
    fn query_interface_view_reuses_cached_view_without_remote_check() {
        let proxy = Arc::new(ObjectProxy::new(Object::new(9)));
        let interface_id = InterfaceOrdinal::new(21);
        let original = proxy.make_interface_view::<FooIface>(interface_id);
        let mut try_cast_calls = 0;

        let result = proxy.query_interface_view::<FooIface, _>(interface_id, true, |_id| {
            try_cast_calls += 1;
            crate::OK()
        });

        assert_eq!(result.error_code, crate::OK());
        let returned = result
            .iface
            .expect("cached interface view should be returned");
        assert!(Arc::ptr_eq(&original, &returned));
        assert_eq!(try_cast_calls, 0);
    }

    #[test]
    fn query_interface_view_returns_invalid_cast_without_creating_view() {
        let proxy = Arc::new(ObjectProxy::new(Object::new(10)));
        let result: QueryInterfaceResult<Arc<RemoteInterfaceView<FooIface>>> = proxy
            .query_interface_view(InterfaceOrdinal::new(99), true, |_id| {
                error_codes::INVALID_CAST()
            });

        assert_eq!(result.error_code, error_codes::INVALID_CAST());
        assert!(result.iface.is_none());
        assert_eq!(proxy.interface_view_count(), 0);
    }
}
