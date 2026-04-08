//! Rust counterpart of `c++/rpc/include/rpc/internal/stub.h`.

use std::any::Any;
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use crate::internal::base::LocalInterfaceView;
use crate::internal::base::RpcObject;
use crate::internal::bindings_fwd::InterfacePointerKind;
use crate::internal::casting_interface::CastingInterface;
use crate::internal::error_codes;
use crate::internal::marshaller_params::{SendParams, SendResult};
use crate::internal::service::Service;
use crate::rpc_types::{CallerZone, InterfaceOrdinal, Object, ReleaseOptions};

pub struct ObjectStub {
    id: Object,
    target: Option<Arc<dyn RpcObject>>,
    target_any: Option<Arc<dyn Any + Send + Sync>>,
    keep_self_alive: Option<Arc<Mutex<ObjectStub>>>,
    shared_count: u64,
    optimistic_count: u64,
    shared_references: BTreeMap<CallerZone, u64>,
    optimistic_references: BTreeMap<CallerZone, u64>,
    owner_service_addr: Option<usize>,
}

impl ObjectStub {
    pub fn new(id: Object) -> Self {
        Self {
            id,
            target: None,
            target_any: None,
            keep_self_alive: None,
            shared_count: 0,
            optimistic_count: 0,
            shared_references: BTreeMap::new(),
            optimistic_references: BTreeMap::new(),
            owner_service_addr: None,
        }
    }

    pub fn with_target<T>(id: Object, target: Arc<T>) -> Self
    where
        T: RpcObject,
    {
        let mut stub = Self::new(id);
        stub.target = Some(target.clone());
        stub.target_any = Some(target);
        stub
    }

    pub fn with_rpc_object(id: Object, target: Arc<dyn RpcObject>) -> Self {
        let mut stub = Self::new(id);
        stub.target = Some(target);
        stub
    }

    pub fn id(&self) -> Object {
        self.id
    }

    pub fn keep_self_alive(&mut self, stub: &Arc<Mutex<ObjectStub>>) {
        self.keep_self_alive = Some(stub.clone());
    }

    pub fn dont_keep_alive(&mut self) {
        self.keep_self_alive = None;
    }

    pub fn set_target<T>(&mut self, target: Arc<T>)
    where
        T: RpcObject,
    {
        self.target = Some(target.clone());
        self.target_any = Some(target);
    }

    pub fn set_rpc_object(&mut self, target: Arc<dyn RpcObject>) {
        self.target = Some(target);
        self.target_any = None;
    }

    pub fn attach_to_service(&mut self, service: &Service) {
        self.owner_service_addr = Some(service as *const Service as usize);
    }

    pub fn detach_from_service(&mut self) {
        self.owner_service_addr = None;
    }

    pub fn owner_service_addr(&self) -> Option<usize> {
        self.owner_service_addr
    }

    pub fn get_castable_interface(
        &self,
        interface_id: InterfaceOrdinal,
    ) -> Option<Arc<dyn CastingInterface>> {
        let target = self.target.as_ref()?;
        if !interface_id.is_set() || target.__rpc_query_interface(interface_id) {
            return Some(target.clone());
        }
        None
    }

    pub fn attach_to_target(&self, stub: &Arc<Mutex<ObjectStub>>) {
        if let Some(target) = &self.target {
            target.__rpc_set_stub(Arc::downgrade(stub));
        }
    }

    pub fn detach_from_target(&self) {
        if let Some(target) = &self.target {
            target.__rpc_set_stub(std::sync::Weak::new());
        }
    }

    pub fn get_local_interface<T>(&self, interface_id: InterfaceOrdinal) -> Option<Arc<T>>
    where
        T: CastingInterface,
    {
        let target = self.target.as_ref()?;
        if interface_id.is_set() && !target.__rpc_query_interface(interface_id) {
            return None;
        }

        self.target_any
            .as_ref()
            .and_then(|target| target.clone().downcast::<T>().ok())
    }

    pub fn get_local_interface_view<T>(&self, interface_id: InterfaceOrdinal) -> Option<Arc<T>>
    where
        T: CastingInterface + ?Sized,
    {
        let target = self.target.as_ref()?;
        if interface_id.is_set() && !target.__rpc_query_interface(interface_id) {
            return None;
        }

        target
            .clone()
            .__rpc_get_local_interface_view(interface_id)
            .and_then(|view| view.downcast::<LocalInterfaceView<T>>().ok())
            .map(|view| view.as_arc())
    }

    pub fn get_local_interface_erased(
        &self,
        interface_id: InterfaceOrdinal,
    ) -> Option<Arc<dyn Any + Send + Sync>> {
        let target = self.target.as_ref()?;
        if interface_id.is_set() && !target.__rpc_query_interface(interface_id) {
            return None;
        }

        target.clone().__rpc_get_local_interface_view(interface_id)
    }

    pub fn call(&self, params: SendParams) -> SendResult {
        if let Some(target) = &self.target {
            return target.__rpc_call(params);
        }

        SendResult::new(error_codes::INVALID_INTERFACE_ID(), vec![], vec![])
    }

    pub fn dispatch_target(&self) -> Option<Arc<dyn RpcObject>> {
        self.target.clone()
    }

    pub fn try_cast(&self, interface_id: InterfaceOrdinal) -> i32 {
        let Some(target) = &self.target else {
            return error_codes::OBJECT_NOT_FOUND();
        };

        if target.__rpc_query_interface(interface_id) {
            return error_codes::OK();
        }

        error_codes::INVALID_CAST()
    }

    pub fn shared_count(&self) -> u64 {
        self.shared_count
    }

    pub fn optimistic_count(&self) -> u64 {
        self.optimistic_count
    }

    pub fn add_ref(
        &mut self,
        pointer_kind: InterfacePointerKind,
        caller_zone_id: CallerZone,
    ) -> u64 {
        let (global_count, references) = match pointer_kind {
            InterfacePointerKind::Shared => (&mut self.shared_count, &mut self.shared_references),
            InterfacePointerKind::Optimistic => {
                (&mut self.optimistic_count, &mut self.optimistic_references)
            }
        };

        *global_count = global_count.saturating_add(1);
        let count = references.entry(caller_zone_id).or_insert(0);
        *count = count.saturating_add(1);
        *global_count
    }

    pub fn release(&mut self, options: ReleaseOptions, caller_zone_id: CallerZone) -> u64 {
        let pointer_kind = if !(options & ReleaseOptions::OPTIMISTIC).is_empty() {
            InterfacePointerKind::Optimistic
        } else {
            InterfacePointerKind::Shared
        };

        let (global_count, references) = match pointer_kind {
            InterfacePointerKind::Shared => (&mut self.shared_count, &mut self.shared_references),
            InterfacePointerKind::Optimistic => {
                (&mut self.optimistic_count, &mut self.optimistic_references)
            }
        };

        if let Some(count) = references.get_mut(&caller_zone_id) {
            *count = count.saturating_sub(1);
            *global_count = global_count.saturating_sub(1);
            if *count == 0 {
                references.remove(&caller_zone_id);
            }
        }

        *global_count
    }

    pub fn has_references_from_zone(&self, caller_zone_id: CallerZone) -> bool {
        self.shared_references.contains_key(&caller_zone_id)
            || self.optimistic_references.contains_key(&caller_zone_id)
    }

    pub fn zones_with_optimistic_refs(&self) -> Vec<CallerZone> {
        self.optimistic_references.keys().cloned().collect()
    }

    pub fn release_all_from_zone(&mut self, caller_zone_id: CallerZone) -> bool {
        if let Some(shared) = self.shared_references.remove(&caller_zone_id) {
            self.shared_count = self.shared_count.saturating_sub(shared);
        }

        if let Some(optimistic) = self.optimistic_references.remove(&caller_zone_id) {
            self.optimistic_count = self.optimistic_count.saturating_sub(optimistic);
        }

        self.shared_count == 0
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Weak};

    use super::ObjectStub;
    use crate::internal::bindings_fwd::InterfacePointerKind;
    use crate::internal::casting_interface::{CastingInterface, GeneratedRustInterface};
    use crate::internal::marshaller_params::{SendParams, SendResult};
    use crate::internal::remote_pointer::LocalProxy;
    use crate::rpc_types::{
        CallerZone, InterfaceOrdinal, Method, Object, ReleaseOptions, ZoneAddress, ZoneAddressArgs,
    };

    fn caller_zone(id: u64) -> CallerZone {
        let mut args = ZoneAddressArgs::default();
        args.subnet = id;
        CallerZone::new(ZoneAddress::create(args).expect("zone address"))
    }

    #[test]
    fn object_stub_tracks_counts_per_zone_and_pointer_kind() {
        let zone_a = caller_zone(1);
        let zone_b = caller_zone(2);
        let mut stub = ObjectStub::new(Object::new(7));

        assert_eq!(
            stub.add_ref(InterfacePointerKind::Shared, zone_a.clone()),
            1
        );
        assert_eq!(
            stub.add_ref(InterfacePointerKind::Optimistic, zone_a.clone()),
            1
        );
        assert_eq!(
            stub.add_ref(InterfacePointerKind::Optimistic, zone_b.clone()),
            2
        );

        assert_eq!(stub.shared_count(), 1);
        assert_eq!(stub.optimistic_count(), 2);
        assert!(stub.has_references_from_zone(zone_a.clone()));
        assert!(stub.has_references_from_zone(zone_b.clone()));
        assert_eq!(
            stub.zones_with_optimistic_refs(),
            vec![zone_a.clone(), zone_b.clone()]
        );

        assert_eq!(stub.release(ReleaseOptions::OPTIMISTIC, zone_a.clone()), 1);
        assert_eq!(stub.release(ReleaseOptions::NORMAL, zone_a.clone()), 0);
        assert!(!stub.has_references_from_zone(zone_a));
        assert!(stub.has_references_from_zone(zone_b));
    }

    #[test]
    fn release_all_from_zone_cleans_shared_and_optimistic_refs() {
        let zone_a = caller_zone(1);
        let zone_b = caller_zone(2);
        let mut stub = ObjectStub::new(Object::new(11));

        stub.add_ref(InterfacePointerKind::Shared, zone_a.clone());
        stub.add_ref(InterfacePointerKind::Optimistic, zone_a.clone());
        stub.add_ref(InterfacePointerKind::Shared, zone_b.clone());

        assert!(!stub.release_all_from_zone(zone_a.clone()));
        assert!(!stub.has_references_from_zone(zone_a));
        assert!(stub.has_references_from_zone(zone_b.clone()));
        assert_eq!(stub.shared_count(), 1);
        assert_eq!(stub.optimistic_count(), 0);

        assert!(stub.release_all_from_zone(zone_b));
        assert_eq!(stub.shared_count(), 0);
    }

    #[test]
    fn release_after_release_all_from_zone_does_not_double_decrement_global_count() {
        let zone_a = caller_zone(1);
        let zone_b = caller_zone(2);
        let mut stub = ObjectStub::new(Object::new(13));

        stub.add_ref(InterfacePointerKind::Shared, zone_a.clone());
        stub.add_ref(InterfacePointerKind::Shared, zone_b.clone());

        assert!(!stub.release_all_from_zone(zone_a.clone()));
        assert_eq!(stub.shared_count(), 1);

        assert_eq!(stub.release(ReleaseOptions::NORMAL, zone_a), 1);
        assert_eq!(stub.shared_count(), 1);

        assert_eq!(stub.release(ReleaseOptions::NORMAL, zone_b), 0);
        assert_eq!(stub.shared_count(), 0);
    }

    #[derive(Debug)]
    struct TestTarget;

    impl crate::internal::remote_pointer::CreateLocalProxy for TestTarget {
        fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
            LocalProxy::new(weak)
        }
    }

    impl CastingInterface for TestTarget {
        fn __rpc_query_interface(&self, interface_id: InterfaceOrdinal) -> bool {
            interface_id == InterfaceOrdinal::new(5)
        }

        fn __rpc_call(&self, params: SendParams) -> SendResult {
            SendResult::new(0, vec![params.method_id.get_val() as u8], vec![])
        }
    }

    impl GeneratedRustInterface for TestTarget {
        fn interface_name() -> &'static str {
            "test::target"
        }

        fn get_id(_rpc_version: u64) -> u64 {
            5
        }

        fn binding_metadata() -> &'static [crate::GeneratedMethodBindingDescriptor] {
            &[]
        }
    }

    #[test]
    fn object_stub_dispatches_calls_and_try_cast_through_target() {
        let target = Arc::new(TestTarget);
        let stub = ObjectStub::with_target(Object::new(99), target);

        assert_eq!(
            stub.try_cast(InterfaceOrdinal::new(5)),
            crate::internal::error_codes::OK()
        );
        assert_eq!(
            stub.try_cast(InterfaceOrdinal::new(6)),
            crate::internal::error_codes::INVALID_CAST()
        );
        assert!(
            stub.get_castable_interface(InterfaceOrdinal::new(5))
                .is_some()
        );
        assert_eq!(
            stub.call(SendParams {
                method_id: Method::new(12),
                ..Default::default()
            })
            .out_buf,
            vec![12]
        );
    }
}
