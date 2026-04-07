pub mod rpc {
    mod generated {
        include!(env!("CANOPY_RPC_PROTO_MESSAGES_RS"));
    }

    pub use generated::*;
}

pub mod __canopy_protobuf {
    pub mod rpc {
        pub use crate::rpc::*;
    }

    pub mod basic_rpc_probe {
        mod generated {
            include!(env!("CANOPY_BASIC_RPC_PROTO_MESSAGES_RS"));
        }

        pub use generated::*;
    }

    pub mod fuzz_test {
        mod generated {
            include!(env!("CANOPY_FUZZ_TEST_PROTO_MESSAGES_RS"));
        }

        pub use generated::*;
    }
}

pub mod basic_rpc_probe {
    include!(env!("CANOPY_BASIC_RPC_BINDINGS_RS"));
}

pub mod basic_rpc_probe_protobuf {
    include!(env!("CANOPY_BASIC_RPC_PROTOBUF_BINDINGS_RS"));
}

pub mod nested_layout_probe {
    include!(env!("CANOPY_NESTED_LAYOUT_BINDINGS_RS"));
}

pub mod fuzz_test {
    include!(env!("CANOPY_FUZZ_TEST_BINDINGS_RS"));
}

pub mod fuzz_test_protobuf {
    include!(env!("CANOPY_FUZZ_TEST_PROTOBUF_BINDINGS_RS"));
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use crate::basic_rpc_probe::probe::{__Generated, IMath, IPeer};
    use __Generated::IMath as i_math;
    use __Generated::IPeer as i_peer;

    mod fuzz_interface_impls {
        use std::sync::Mutex;

        use crate::fuzz_test::fuzz_test::{self as fuzz, __Generated as fg};

        macro_rules! impl_fuzz_runtime_interface {
            ($ty:ty, $module:ident) => {
                impl canopy_rpc::CreateLocalProxy for $ty {}

                impl canopy_rpc::CastingInterface for $ty {
                    fn __rpc_query_interface(
                        &self,
                        interface_id: canopy_rpc::InterfaceOrdinal,
                    ) -> bool {
                        fg::$module::matches_interface_id(interface_id)
                    }
                }

                impl canopy_rpc::GeneratedRustInterface for $ty {
                    fn interface_name() -> &'static str {
                        fg::$module::NAME
                    }

                    fn get_id(rpc_version: u64) -> u64 {
                        if rpc_version >= 3 {
                            fg::$module::ID_RPC_V3
                        } else {
                            fg::$module::ID_RPC_V2
                        }
                    }

                    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor]
                    {
                        fg::$module::interface_binding::METHODS
                    }
                }
            };
        }

        pub(super) struct CleanupImpl;
        pub(super) struct GarbageCollectorImpl;

        #[derive(Default)]
        pub(super) struct SharedObjectImpl {
            value: Mutex<i32>,
            calls: Mutex<i32>,
        }

        pub(super) struct FuzzFactoryImpl;
        pub(super) struct FuzzCacheImpl;
        pub(super) struct FuzzWorkerImpl;
        pub(super) struct AutonomousNodeImpl;
        pub(super) struct FuzzControllerImpl;

        impl_fuzz_runtime_interface!(CleanupImpl, ICleanup);
        impl_fuzz_runtime_interface!(GarbageCollectorImpl, IGarbageCollector);
        impl_fuzz_runtime_interface!(SharedObjectImpl, ISharedObject);
        impl_fuzz_runtime_interface!(FuzzFactoryImpl, IFuzzFactory);
        impl_fuzz_runtime_interface!(FuzzCacheImpl, IFuzzCache);
        impl_fuzz_runtime_interface!(FuzzWorkerImpl, IFuzzWorker);
        impl_fuzz_runtime_interface!(AutonomousNodeImpl, IAutonomousNode);
        impl_fuzz_runtime_interface!(FuzzControllerImpl, IFuzzController);

        impl fuzz::ICleanup for CleanupImpl {
            type CleanupCollectorIface = GarbageCollectorImpl;

            fn cleanup<CollectorIface>(
                &self,
                _collector: canopy_rpc::Shared<std::sync::Arc<CollectorIface>>,
            ) -> i32
            where
                CollectorIface: fuzz::IGarbageCollector,
            {
                canopy_rpc::OK()
            }
        }

        impl fuzz::IGarbageCollector for GarbageCollectorImpl {
            type CollectObjIface = CleanupImpl;

            fn collect<ObjIface>(&self, _obj: canopy_rpc::Shared<std::sync::Arc<ObjIface>>) -> i32
            where
                ObjIface: fuzz::ICleanup,
            {
                canopy_rpc::OK()
            }

            fn get_collected_count(&self, count: &mut i32) -> i32 {
                *count = 0;
                canopy_rpc::OK()
            }
        }

        impl fuzz::ISharedObject for SharedObjectImpl {
            fn test_function(&self, input_value: i32) -> i32 {
                *self.calls.lock().expect("shared object calls") += 1;
                input_value
            }

            fn get_stats(&self, count: &mut i32) -> i32 {
                *count = *self.calls.lock().expect("shared object calls");
                canopy_rpc::OK()
            }

            fn set_value(&self, new_value: i32) -> i32 {
                *self.value.lock().expect("shared object value") = new_value;
                canopy_rpc::OK()
            }

            fn get_value(&self, value: &mut i32) -> i32 {
                *value = *self.value.lock().expect("shared object value");
                canopy_rpc::OK()
            }
        }

        impl fuzz::IFuzzFactory for FuzzFactoryImpl {
            type CreateSharedObjectCreatedObjectIface = SharedObjectImpl;
            type PlaceSharedObjectNewObjectIface = SharedObjectImpl;
            type PlaceSharedObjectTargetObjectIface = SharedObjectImpl;

            fn create_shared_object<CreatedObjectIface>(
                &self,
                _id: i32,
                _name: String,
                _initial_value: i32,
                created_object: &mut canopy_rpc::Shared<std::sync::Arc<CreatedObjectIface>>,
            ) -> i32
            where
                CreatedObjectIface: fuzz::ISharedObject,
            {
                *created_object = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn place_shared_object<NewObjectIface, TargetObjectIface>(
                &self,
                _new_object: canopy_rpc::Shared<std::sync::Arc<NewObjectIface>>,
                _target_object: canopy_rpc::Shared<std::sync::Arc<TargetObjectIface>>,
            ) -> i32
            where
                NewObjectIface: fuzz::ISharedObject,
                TargetObjectIface: fuzz::ISharedObject,
            {
                canopy_rpc::OK()
            }

            fn get_factory_stats(&self, total_created: &mut i32, current_refs: &mut i32) -> i32 {
                *total_created = 0;
                *current_refs = 0;
                canopy_rpc::OK()
            }
        }

        impl fuzz::IFuzzCache for FuzzCacheImpl {
            type StoreObjectObjectIface = SharedObjectImpl;
            type RetrieveObjectObjectIface = SharedObjectImpl;

            fn store_object<ObjectIface>(
                &self,
                _cache_key: i32,
                _object: canopy_rpc::Shared<std::sync::Arc<ObjectIface>>,
            ) -> i32
            where
                ObjectIface: fuzz::ISharedObject,
            {
                canopy_rpc::OK()
            }

            fn retrieve_object<ObjectIface>(
                &self,
                _cache_key: i32,
                object: &mut canopy_rpc::Shared<std::sync::Arc<ObjectIface>>,
            ) -> i32
            where
                ObjectIface: fuzz::ISharedObject,
            {
                *object = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn has_object(&self, _cache_key: i32, exists: &mut bool) -> i32 {
                *exists = false;
                canopy_rpc::OK()
            }

            fn get_cache_size(&self, size: &mut i32) -> i32 {
                *size = 0;
                canopy_rpc::OK()
            }
        }

        impl fuzz::IFuzzWorker for FuzzWorkerImpl {
            type ProcessObjectObjectIface = SharedObjectImpl;

            fn process_object<ObjectIface>(
                &self,
                _object: canopy_rpc::Shared<std::sync::Arc<ObjectIface>>,
                _increment: i32,
            ) -> i32
            where
                ObjectIface: fuzz::ISharedObject,
            {
                canopy_rpc::OK()
            }

            fn get_worker_stats(
                &self,
                objects_processed: &mut i32,
                total_increments: &mut i32,
            ) -> i32 {
                *objects_processed = 0;
                *total_increments = 0;
                canopy_rpc::OK()
            }
        }

        impl fuzz::IAutonomousNode for AutonomousNodeImpl {
            type RunScriptTargetNodeIface = AutonomousNodeImpl;
            type ExecuteInstructionInputObjectIface = SharedObjectImpl;
            type ExecuteInstructionOutputObjectIface = SharedObjectImpl;
            type ConnectToNodeTargetNodeIface = AutonomousNodeImpl;
            type PassObjectToConnectedObjectIface = SharedObjectImpl;
            type ReceiveObjectObjectIface = SharedObjectImpl;
            type CreateChildNodeChildNodeIface = AutonomousNodeImpl;
            type RequestChildCreationTargetParentIface = AutonomousNodeImpl;
            type RequestChildCreationChildProxyIface = AutonomousNodeImpl;
            type GetCachedChildByIndexChildIface = AutonomousNodeImpl;
            type GetParentNodeParentIface = AutonomousNodeImpl;
            type SetParentNodeParentIface = AutonomousNodeImpl;

            fn initialize_node(&self, _type: canopy_rpc::OpaqueValue, _node_id: u64) -> i32 {
                canopy_rpc::OK()
            }

            fn run_script<TargetNodeIface>(
                &self,
                _target_node: canopy_rpc::Shared<std::sync::Arc<TargetNodeIface>>,
                _instruction_count: i32,
            ) -> i32
            where
                TargetNodeIface: fuzz::IAutonomousNode,
            {
                canopy_rpc::OK()
            }

            fn execute_instruction<InputObjectIface, OutputObjectIface>(
                &self,
                _instruction: fuzz::Instruction,
                _input_object: canopy_rpc::Shared<std::sync::Arc<InputObjectIface>>,
                output_object: &mut canopy_rpc::Shared<std::sync::Arc<OutputObjectIface>>,
            ) -> i32
            where
                InputObjectIface: fuzz::ISharedObject,
                OutputObjectIface: fuzz::ISharedObject,
            {
                *output_object = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn connect_to_node<TargetNodeIface>(
                &self,
                _target_node: canopy_rpc::Shared<std::sync::Arc<TargetNodeIface>>,
            ) -> i32
            where
                TargetNodeIface: fuzz::IAutonomousNode,
            {
                canopy_rpc::OK()
            }

            fn pass_object_to_connected<ObjectIface>(
                &self,
                _connection_index: i32,
                _object: canopy_rpc::Shared<std::sync::Arc<ObjectIface>>,
            ) -> i32
            where
                ObjectIface: fuzz::ISharedObject,
            {
                canopy_rpc::OK()
            }

            fn receive_object<ObjectIface>(
                &self,
                _object: canopy_rpc::Shared<std::sync::Arc<ObjectIface>>,
                _sender_node_id: u64,
            ) -> i32
            where
                ObjectIface: fuzz::ISharedObject,
            {
                canopy_rpc::OK()
            }

            fn get_node_status(
                &self,
                current_type: &mut canopy_rpc::OpaqueValue,
                current_id: &mut u64,
                connections_count: &mut i32,
                objects_held: &mut i32,
            ) -> i32 {
                *current_type = canopy_rpc::OpaqueValue;
                *current_id = 0;
                *connections_count = 0;
                *objects_held = 0;
                canopy_rpc::OK()
            }

            fn create_child_node<ChildNodeIface>(
                &self,
                _child_type: canopy_rpc::OpaqueValue,
                _child_zone_id: u64,
                _cache_locally: bool,
                child_node: &mut canopy_rpc::Shared<std::sync::Arc<ChildNodeIface>>,
            ) -> i32
            where
                ChildNodeIface: fuzz::IAutonomousNode,
            {
                *child_node = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn request_child_creation<TargetParentIface, ChildProxyIface>(
                &self,
                _target_parent: canopy_rpc::Shared<std::sync::Arc<TargetParentIface>>,
                _child_type: canopy_rpc::OpaqueValue,
                _child_zone_id: u64,
                child_proxy: &mut canopy_rpc::Shared<std::sync::Arc<ChildProxyIface>>,
            ) -> i32
            where
                TargetParentIface: fuzz::IAutonomousNode,
                ChildProxyIface: fuzz::IAutonomousNode,
            {
                *child_proxy = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn get_cached_children_count(&self, count: &mut i32) -> i32 {
                *count = 0;
                canopy_rpc::OK()
            }

            fn get_cached_child_by_index<ChildIface>(
                &self,
                _index: i32,
                child: &mut canopy_rpc::Shared<std::sync::Arc<ChildIface>>,
            ) -> i32
            where
                ChildIface: fuzz::IAutonomousNode,
            {
                *child = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn get_parent_node<ParentIface>(
                &self,
                parent: &mut canopy_rpc::Shared<std::sync::Arc<ParentIface>>,
            ) -> i32
            where
                ParentIface: fuzz::IAutonomousNode,
            {
                *parent = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn set_parent_node<ParentIface>(
                &self,
                _parent: canopy_rpc::Shared<std::sync::Arc<ParentIface>>,
            ) -> i32
            where
                ParentIface: fuzz::IAutonomousNode,
            {
                canopy_rpc::OK()
            }
        }

        impl fuzz::IFuzzController for FuzzControllerImpl {
            type CreateZoneWithNodeCreatedNodeIface = AutonomousNodeImpl;
            type CreatePitchforkConnectionRootIface = AutonomousNodeImpl;
            type CreatePitchforkConnectionLeftBranchIface = AutonomousNodeImpl;
            type CreatePitchforkConnectionRightBranchIface = AutonomousNodeImpl;

            fn create_zone_with_node<CreatedNodeIface>(
                &self,
                _type: canopy_rpc::OpaqueValue,
                _zone_id: u64,
                created_node: &mut canopy_rpc::Shared<std::sync::Arc<CreatedNodeIface>>,
            ) -> i32
            where
                CreatedNodeIface: fuzz::IAutonomousNode,
            {
                *created_node = canopy_rpc::Shared::null();
                canopy_rpc::OK()
            }

            fn create_pitchfork_connection<RootIface, LeftBranchIface, RightBranchIface>(
                &self,
                _root: canopy_rpc::Shared<std::sync::Arc<RootIface>>,
                _left_branch: canopy_rpc::Shared<std::sync::Arc<LeftBranchIface>>,
                _right_branch: canopy_rpc::Shared<std::sync::Arc<RightBranchIface>>,
            ) -> i32
            where
                RootIface: fuzz::IAutonomousNode,
                LeftBranchIface: fuzz::IAutonomousNode,
                RightBranchIface: fuzz::IAutonomousNode,
            {
                canopy_rpc::OK()
            }

            fn execute_fuzz_iteration(
                &self,
                _iteration_id: i32,
                _num_objects_to_create: i32,
                _num_operations: i32,
            ) -> i32 {
                canopy_rpc::OK()
            }

            fn get_test_stats(
                &self,
                total_zones_created: &mut i32,
                total_objects_created: &mut i32,
                total_operations: &mut i32,
                total_ref_count_changes: &mut i32,
            ) -> i32 {
                *total_zones_created = 0;
                *total_objects_created = 0;
                *total_operations = 0;
                *total_ref_count_changes = 0;
                canopy_rpc::OK()
            }

            fn cleanup_test_resources(&self) -> i32 {
                canopy_rpc::OK()
            }
        }
    }

    fn zone(subnet: u64) -> canopy_rpc::Zone {
        let mut args = canopy_rpc::ZoneAddressArgs::default();
        args.subnet = subnet;
        canopy_rpc::Zone::from(canopy_rpc::ZoneAddress::create(args).expect("zone address"))
    }

    fn to_proto_remote_object(
        value: &canopy_rpc::RemoteObject,
    ) -> crate::__canopy_protobuf::rpc::remote_object {
        let mut zone_address = crate::__canopy_protobuf::rpc::zone_address::new();
        zone_address.set_blob(value.get_address().get_blob().to_vec());

        let mut remote_object = crate::__canopy_protobuf::rpc::remote_object::new();
        remote_object.set_addr_(zone_address);
        remote_object
    }

    #[test]
    fn generated_rust_layout_uses_camel_case_public_api_and_innermost_generated_module() {
        use crate::nested_layout_probe::outer::inner::{__Generated, IWidget, WidgetState};

        fn assert_widget_trait<T: IWidget>() {}
        fn assert_generated_proxy_skeleton<T>()
        where
            T: canopy_rpc::CreateLocalProxy
                + canopy_rpc::CastingInterface
                + canopy_rpc::GeneratedRustInterface,
        {
        }

        assert_widget_trait::<__Generated::IWidget::ProxySkeleton>();
        assert_generated_proxy_skeleton::<__Generated::IWidget::ProxySkeleton>();

        let state = WidgetState::default();
        assert_eq!(state.count, 0);
        assert_eq!(__Generated::IWidget::NAME, "i_widget");
    }

    #[test]
    fn rust_implements_moved_fuzz_interfaces() {
        use crate::fuzz_test::fuzz_test::{self as fuzz, __Generated as fg};
        use fuzz::ISharedObject as _;

        use fuzz_interface_impls::{
            AutonomousNodeImpl, CleanupImpl, FuzzCacheImpl, FuzzControllerImpl, FuzzFactoryImpl,
            FuzzWorkerImpl, GarbageCollectorImpl, SharedObjectImpl,
        };

        fn assert_cleanup<T: fuzz::ICleanup>() {}
        fn assert_collector<T: fuzz::IGarbageCollector>() {}
        fn assert_shared_object<T: fuzz::ISharedObject>() {}
        fn assert_factory<T: fuzz::IFuzzFactory>() {}
        fn assert_cache<T: fuzz::IFuzzCache>() {}
        fn assert_worker<T: fuzz::IFuzzWorker>() {}
        fn assert_node<T: fuzz::IAutonomousNode>() {}
        fn assert_controller<T: fuzz::IFuzzController>() {}

        assert_cleanup::<CleanupImpl>();
        assert_collector::<GarbageCollectorImpl>();
        assert_shared_object::<SharedObjectImpl>();
        assert_factory::<FuzzFactoryImpl>();
        assert_cache::<FuzzCacheImpl>();
        assert_worker::<FuzzWorkerImpl>();
        assert_node::<AutonomousNodeImpl>();
        assert_controller::<FuzzControllerImpl>();

        let shared_object = SharedObjectImpl::default();
        assert_eq!(shared_object.set_value(42), canopy_rpc::OK());

        let mut value = 0;
        assert_eq!(shared_object.get_value(&mut value), canopy_rpc::OK());
        assert_eq!(value, 42);

        assert_eq!(shared_object.test_function(7), 7);

        let mut count = 0;
        assert_eq!(shared_object.get_stats(&mut count), canopy_rpc::OK());
        assert_eq!(count, 1);

        assert_eq!(fg::ISharedObject::NAME, "i_shared_object");
        assert!(fg::ISharedObject::matches_interface_id(
            canopy_rpc::InterfaceOrdinal::new(fg::ISharedObject::ID_RPC_V3)
        ));
    }

    struct MathImpl;

    pub(super) struct PeerImpl;

    impl canopy_rpc::CreateLocalProxy for PeerImpl {}

    impl canopy_rpc::CastingInterface for PeerImpl {
        fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
            i_peer::matches_interface_id(interface_id)
        }

        fn __rpc_call(&self, params: canopy_rpc::SendParams) -> canopy_rpc::SendResult {
            let context = canopy_rpc::DispatchContext::from(&params);
            <Self as IPeer>::__rpc_dispatch_generated(self, &context, params)
        }
    }

    impl canopy_rpc::GeneratedRustInterface for PeerImpl {
        fn interface_name() -> &'static str {
            i_peer::NAME
        }

        fn get_id(rpc_version: u64) -> u64 {
            if rpc_version >= 3 {
                i_peer::ID_RPC_V3
            } else {
                i_peer::ID_RPC_V2
            }
        }

        fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
            i_peer::interface_binding::METHODS
        }
    }

    impl IPeer for PeerImpl {
        fn ping(&self, value: &mut i32) -> i32 {
            *value = 7;
            canopy_rpc::OK()
        }
    }

    impl canopy_rpc::CreateLocalProxy for MathImpl {}

    impl canopy_rpc::CastingInterface for MathImpl {
        fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
            i_math::matches_interface_id(interface_id)
        }
    }

    impl canopy_rpc::GeneratedRustInterface for MathImpl {
        fn interface_name() -> &'static str {
            i_math::NAME
        }

        fn get_id(rpc_version: u64) -> u64 {
            if rpc_version >= 3 {
                i_math::ID_RPC_V3
            } else {
                i_math::ID_RPC_V2
            }
        }

        fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
            i_math::interface_binding::METHODS
        }
    }

    impl IMath for MathImpl {
        type AcceptSharedPeerPeerIface = PeerImpl;
        type AcceptOptimisticPeerPeerIface = PeerImpl;
        type CreateSharedPeerCreatedPeerIface = PeerImpl;
        type CreateOptimisticPeerCreatedPeerIface = PeerImpl;
        type EchoSharedPeerInputIface = PeerImpl;
        type EchoSharedPeerOutputIface = PeerImpl;
        type EchoOptimisticPeerInputIface = PeerImpl;
        type EchoOptimisticPeerOutputIface = PeerImpl;

        fn add(&self, a: i32, b: i32, c: &mut i32) -> i32 {
            *c = a + b;
            canopy_rpc::OK()
        }

        fn bounce_text(&self, input: String, output: &mut String) -> i32 {
            *output = format!("echo:{input}");
            canopy_rpc::OK()
        }

        fn reverse_sequence(&self, input: Vec<u64>, output: &mut Vec<u64>) -> i32 {
            *output = input.into_iter().rev().collect();
            canopy_rpc::OK()
        }

        fn increment_sequence(&self, input: Vec<i32>, output: &mut Vec<i32>) -> i32 {
            *output = input.into_iter().map(|value| value + 1).collect();
            canopy_rpc::OK()
        }

        fn decorate_many(&self, input: Vec<String>, output: &mut Vec<String>) -> i32 {
            *output = input
                .into_iter()
                .map(|value| format!("item:{value}"))
                .collect();
            canopy_rpc::OK()
        }

        fn echo_bytes(&self, input: Vec<u8>, output: &mut Vec<u8>) -> i32 {
            *output = input;
            canopy_rpc::OK()
        }

        fn negate_signed_bytes(&self, input: Vec<i8>, output: &mut Vec<i8>) -> i32 {
            *output = input.into_iter().map(|value| -value).collect();
            canopy_rpc::OK()
        }

        fn translate_point(
            &self,
            p: crate::basic_rpc_probe::probe::Point,
            dx: i32,
            dy: i32,
            translated: &mut crate::basic_rpc_probe::probe::Point,
        ) -> i32 {
            *translated = crate::basic_rpc_probe::probe::Point {
                x: p.x + dx,
                y: p.y + dy,
            };
            canopy_rpc::OK()
        }

        fn label_value(
            &self,
            input: crate::basic_rpc_probe::probe::LabeledValue,
            output: &mut crate::basic_rpc_probe::probe::LabeledValue,
        ) -> i32 {
            *output = crate::basic_rpc_probe::probe::LabeledValue {
                label: format!("[{}]", input.label),
                value: input.value * 2,
            };
            canopy_rpc::OK()
        }

        fn accept_shared_peer<PeerIface>(
            &self,
            peer: canopy_rpc::Shared<Arc<PeerIface>>,
            seen: &mut i32,
        ) -> i32
        where
            PeerIface: IPeer,
        {
            let canopy_rpc::BoundInterface::Value(peer) = peer.into_inner() else {
                *seen = -1;
                return canopy_rpc::OK();
            };
            let mut value = 0;
            let result = peer.ping(&mut value);
            if result == canopy_rpc::OK() {
                *seen = value + 100;
            }
            result
        }

        fn accept_optimistic_peer<PeerIface>(
            &self,
            peer: canopy_rpc::Optimistic<canopy_rpc::LocalProxy<PeerIface>>,
            seen: &mut i32,
        ) -> i32
        where
            PeerIface: IPeer,
        {
            let canopy_rpc::BoundInterface::Value(peer) = peer.into_inner() else {
                *seen = -1;
                return canopy_rpc::OK();
            };
            let Some(peer) = peer.upgrade() else {
                *seen = -2;
                return canopy_rpc::OBJECT_GONE();
            };
            let mut value = 0;
            let result = peer.ping(&mut value);
            if result == canopy_rpc::OK() {
                *seen = value + 200;
            }
            result
        }

        fn create_shared_peer<CreatedPeerIface>(
            &self,
            _created_peer: &mut canopy_rpc::Shared<Arc<CreatedPeerIface>>,
        ) -> i32
        where
            CreatedPeerIface: IPeer,
        {
            canopy_rpc::INVALID_DATA()
        }

        fn create_optimistic_peer<CreatedPeerIface>(
            &self,
            _created_peer: &mut canopy_rpc::Optimistic<canopy_rpc::LocalProxy<CreatedPeerIface>>,
        ) -> i32
        where
            CreatedPeerIface: IPeer,
        {
            canopy_rpc::INVALID_DATA()
        }

        fn echo_shared_peer<InputIface, OutputIface>(
            &self,
            _input: canopy_rpc::Shared<Arc<InputIface>>,
            _output: &mut canopy_rpc::Shared<Arc<OutputIface>>,
        ) -> i32
        where
            InputIface: IPeer,
            OutputIface: IPeer,
        {
            canopy_rpc::INVALID_DATA()
        }

        fn echo_optimistic_peer<InputIface, OutputIface>(
            &self,
            _input: canopy_rpc::Optimistic<canopy_rpc::LocalProxy<InputIface>>,
            _output: &mut canopy_rpc::Optimistic<canopy_rpc::LocalProxy<OutputIface>>,
        ) -> i32
        where
            InputIface: IPeer,
            OutputIface: IPeer,
        {
            canopy_rpc::INVALID_DATA()
        }
    }

    #[test]
    fn generated_idl_proxy_round_trips_over_local_protobuf_rpc() {
        let service = Arc::new(canopy_rpc::Service::new("probe", zone(1)));
        let object_id = service.generate_new_object_id();
        let rpc_object = i_math::make_rpc_object(MathImpl);
        let stub = Arc::new(Mutex::new(canopy_rpc::ObjectStub::with_target(
            object_id, rpc_object,
        )));
        assert_eq!(service.register_stub(&stub), canopy_rpc::OK());

        let remote_object = service
            .zone_id()
            .with_object(object_id)
            .expect("remote object descriptor");
        let caller_zone_id = canopy_rpc::CallerZone::from(service.zone_id().get_address().clone());

        let proxy = i_math::ProxySkeleton::with_caller(canopy_rpc::ServiceProxy::local(
            service.clone(),
            canopy_rpc::GeneratedRpcCallContext {
                protocol_version: canopy_rpc::get_version(),
                encoding_type: canopy_rpc::Encoding::ProtocolBuffers,
                tag: 0,
                caller_zone_id,
                remote_object_id: remote_object.clone(),
            },
        ));

        let mut sum = -1;
        let result = proxy.add(20, 22, &mut sum);

        assert_eq!(result, canopy_rpc::OK());
        assert_eq!(sum, 42);

        let mut echoed = String::new();
        let text_result = proxy.bounce_text("probe".to_string(), &mut echoed);

        assert_eq!(text_result, canopy_rpc::OK());
        assert_eq!(echoed, "echo:probe");

        let mut reversed = Vec::new();
        let sequence_result = proxy.reverse_sequence(vec![1, 2, 3, 4], &mut reversed);

        assert_eq!(sequence_result, canopy_rpc::OK());
        assert_eq!(reversed, vec![4, 3, 2, 1]);

        let mut incremented = Vec::new();
        let increment_result = proxy.increment_sequence(vec![4, 9, 12], &mut incremented);

        assert_eq!(increment_result, canopy_rpc::OK());
        assert_eq!(incremented, vec![5, 10, 13]);

        let mut decorated = Vec::new();
        let decorate_result =
            proxy.decorate_many(vec!["a".to_string(), "probe".to_string()], &mut decorated);

        assert_eq!(decorate_result, canopy_rpc::OK());
        assert_eq!(
            decorated,
            vec!["item:a".to_string(), "item:probe".to_string()]
        );

        let mut echoed_bytes = Vec::new();
        let echo_bytes_result = proxy.echo_bytes(vec![1, 7, 9, 255], &mut echoed_bytes);

        assert_eq!(echo_bytes_result, canopy_rpc::OK());
        assert_eq!(echoed_bytes, vec![1, 7, 9, 255]);

        let mut negated_bytes = Vec::new();
        let negate_bytes_result = proxy.negate_signed_bytes(vec![-4, 0, 9], &mut negated_bytes);

        assert_eq!(negate_bytes_result, canopy_rpc::OK());
        assert_eq!(negated_bytes, vec![4, 0, -9]);

        let p = crate::basic_rpc_probe::probe::Point { x: 3, y: 7 };
        let mut translated = crate::basic_rpc_probe::probe::Point::default();
        let translate_result = proxy.translate_point(p, 10, -2, &mut translated);

        assert_eq!(translate_result, canopy_rpc::OK());
        assert_eq!(translated.x, 13);
        assert_eq!(translated.y, 5);

        let lv = crate::basic_rpc_probe::probe::LabeledValue {
            label: "hello".to_string(),
            value: 21,
        };
        let mut lv_out = crate::basic_rpc_probe::probe::LabeledValue::default();
        let label_result = proxy.label_value(lv, &mut lv_out);

        assert_eq!(label_result, canopy_rpc::OK());
        assert_eq!(lv_out.label, "[hello]");
        assert_eq!(lv_out.value, 42);

        let proxy_peer = Arc::new(PeerImpl);
        let mut seen_shared = -1;
        let accept_shared_result = proxy.accept_shared_peer(
            canopy_rpc::Shared::from_value(proxy_peer.clone()),
            &mut seen_shared,
        );
        assert_eq!(accept_shared_result, canopy_rpc::OK());
        assert_eq!(seen_shared, 107);

        let proxy_peer_view = canopy_rpc::LocalProxy::from_shared(&proxy_peer);
        let mut seen_optimistic = -1;
        let accept_optimistic_result = proxy.accept_optimistic_peer(
            canopy_rpc::Optimistic::from_value(proxy_peer_view),
            &mut seen_optimistic,
        );
        assert_eq!(accept_optimistic_result, canopy_rpc::OK());
        assert_eq!(seen_optimistic, 207);

        let peer_object_id = service.generate_new_object_id();
        let _peer_stub = service
            .register_local_object(peer_object_id, Arc::new(PeerImpl))
            .expect("peer stub registration");
        let peer_remote_object = service
            .zone_id()
            .with_object(peer_object_id)
            .expect("peer remote object descriptor");

        let mut shared_request =
            crate::__canopy_protobuf::basic_rpc_probe::i_math_accept_shared_peerRequest::new();
        shared_request.set_peer(to_proto_remote_object(&peer_remote_object));
        let shared_params = canopy_rpc::SendParams {
            protocol_version: canopy_rpc::get_version(),
            caller_zone_id: canopy_rpc::CallerZone::from(service.zone_id().get_address().clone()),
            remote_object_id: remote_object.clone(),
            interface_id: canopy_rpc::InterfaceOrdinal::new(i_math::ID_RPC_V3),
            method_id: canopy_rpc::Method::new(i_math::methods::ACCEPT_SHARED_PEER),
            encoding_type: canopy_rpc::Encoding::ProtocolBuffers,
            tag: 0,
            in_data: protobuf::Serialize::serialize(&shared_request).expect("shared request"),
            in_back_channel: vec![],
        };
        let shared_result = service.send(shared_params);
        assert_eq!(shared_result.error_code, canopy_rpc::OK());
        let shared_response: crate::__canopy_protobuf::basic_rpc_probe::i_math_accept_shared_peerResponse =
            protobuf::Parse::parse(&shared_result.out_buf).expect("shared response");
        assert_eq!(shared_response.result(), canopy_rpc::OK());
        assert_eq!(shared_response.seen(), 107);

        let mut optimistic_request =
            crate::__canopy_protobuf::basic_rpc_probe::i_math_accept_optimistic_peerRequest::new();
        optimistic_request.set_peer(to_proto_remote_object(&peer_remote_object));
        let optimistic_params = canopy_rpc::SendParams {
            protocol_version: canopy_rpc::get_version(),
            caller_zone_id: canopy_rpc::CallerZone::from(service.zone_id().get_address().clone()),
            remote_object_id: remote_object,
            interface_id: canopy_rpc::InterfaceOrdinal::new(i_math::ID_RPC_V3),
            method_id: canopy_rpc::Method::new(i_math::methods::ACCEPT_OPTIMISTIC_PEER),
            encoding_type: canopy_rpc::Encoding::ProtocolBuffers,
            tag: 0,
            in_data: protobuf::Serialize::serialize(&optimistic_request)
                .expect("optimistic request"),
            in_back_channel: vec![],
        };
        let optimistic_result = service.send(optimistic_params);
        assert_eq!(optimistic_result.error_code, canopy_rpc::OK());
        let optimistic_response: crate::__canopy_protobuf::basic_rpc_probe::i_math_accept_optimistic_peerResponse =
            protobuf::Parse::parse(&optimistic_result.out_buf).expect("optimistic response");
        assert_eq!(optimistic_response.result(), canopy_rpc::OK());
        assert_eq!(optimistic_response.seen(), 207);
    }

    #[test]
    fn interface_pointer_kind_metadata_distinguishes_shared_from_optimistic() {
        use crate::basic_rpc_probe_protobuf::probe::__Generated::IMath::interface_binding::{
            accept_optimistic_peer, accept_shared_peer,
        };
        use canopy_rpc::serialization::protobuf::{
            GeneratedProtobufFieldKind, InterfacePointerKind,
        };

        // Verify accept_shared_peer has Shared pointer kind
        let accept_shared_params = accept_shared_peer::PROTOBUF_PARAMS;
        assert_eq!(accept_shared_params.len(), 2);
        assert_eq!(accept_shared_params[0].name, "peer");
        assert_eq!(
            accept_shared_params[0].field_kind,
            GeneratedProtobufFieldKind::InterfaceRemoteObject
        );
        assert_eq!(
            accept_shared_params[0].pointer_kind,
            Some(InterfacePointerKind::Shared)
        );
        assert_eq!(accept_shared_params[1].name, "seen");
        assert_eq!(accept_shared_params[1].pointer_kind, None);

        // Verify accept_optimistic_peer has Optimistic pointer kind
        let accept_optimistic_params = accept_optimistic_peer::PROTOBUF_PARAMS;
        assert_eq!(accept_optimistic_params.len(), 2);
        assert_eq!(accept_optimistic_params[0].name, "peer");
        assert_eq!(
            accept_optimistic_params[0].field_kind,
            GeneratedProtobufFieldKind::InterfaceRemoteObject
        );
        assert_eq!(
            accept_optimistic_params[0].pointer_kind,
            Some(InterfacePointerKind::Optimistic)
        );
        assert_eq!(accept_optimistic_params[1].name, "seen");
        assert_eq!(accept_optimistic_params[1].pointer_kind, None);
    }
}

// Tests for the reverse cross-language proof:
// generated Rust proxy -> C ABI -> generated C++ protobuf object.
#[cfg(test)]
mod cxx_dll_tests {
    use std::collections::HashMap;
    use std::ffi::c_void;
    use std::sync::{Arc, Mutex};

    use crate::basic_rpc_probe::probe::{__Generated, IMath, IPeer};
    use __Generated::IMath as i_math;
    use __Generated::IPeer as i_peer;
    use canopy_rpc::internal::error_codes;
    use canopy_rpc::{
        AddRefParams, AddressType, DefaultValues, Encoding, GetNewZoneIdParams, IMarshaller,
        NewZoneIdResult, ObjectReleasedParams, PostParams, ReleaseParams, SendParams, SendResult,
        StandardResult, TransportDownParams, TryCastParams, Zone, ZoneAddress, ZoneAddressArgs,
    };
    use canopy_transport_dynamic_library as dll;
    use canopy_transport_dynamic_library::ffi::{
        CanopyAllocatorVtable, CanopyByteBuffer, CanopyConnectionSettings, CanopyDllInitParams,
        CanopyRemoteObject,
    };

    static CXX_DLL_TEST_LOCK: Mutex<()> = Mutex::new(());

    struct AllocState {
        allocations: HashMap<usize, Box<[u8]>>,
    }

    struct ParentCallbackState {
        next_zone_subnet: u64,
        allocator: CanopyAllocatorVtable,
        service: Arc<canopy_rpc::Service>,
    }

    unsafe extern "C" fn test_alloc(ctx: *mut c_void, size: usize) -> CanopyByteBuffer {
        let state = unsafe { &mut *(ctx as *mut AllocState) };
        let mut data = vec![0u8; size].into_boxed_slice();
        let ptr = data.as_mut_ptr();
        state.allocations.insert(ptr as usize, data);
        CanopyByteBuffer { data: ptr, size }
    }

    unsafe extern "C" fn test_free(ctx: *mut c_void, data: *mut u8, _size: usize) {
        let state = unsafe { &mut *(ctx as *mut AllocState) };
        state.allocations.remove(&(data as usize));
    }

    unsafe extern "C" fn parent_send(
        parent_ctx: *mut c_void,
        params: *const dll::ffi::CanopySendParams,
        result: *mut dll::ffi::CanopySendResult,
    ) -> i32 {
        if parent_ctx.is_null() || params.is_null() || result.is_null() {
            return error_codes::INVALID_DATA();
        }

        let state = unsafe { &mut *(parent_ctx as *mut ParentCallbackState) };
        let params = match dll::ffi::copy_send_params(unsafe { &*params }) {
            Ok(params) => params,
            Err(error_code) => {
                unsafe {
                    (*result).error_code = error_code;
                }
                return error_code;
            }
        };
        let value = state.service.send(params);

        match dll::ffi::write_send_result(&state.allocator, &value, unsafe { &mut *result }) {
            Ok(()) => error_codes::OK(),
            Err(error_code) => {
                unsafe {
                    (*result).error_code = error_code;
                }
                error_code
            }
        }
    }

    unsafe extern "C" fn parent_try_cast(
        parent_ctx: *mut c_void,
        params: *const dll::ffi::CanopyTryCastParams,
        result: *mut dll::ffi::CanopyStandardResult,
    ) -> i32 {
        if parent_ctx.is_null() || params.is_null() || result.is_null() {
            return error_codes::INVALID_DATA();
        }

        let state = unsafe { &mut *(parent_ctx as *mut ParentCallbackState) };
        let params = dll::ffi::copy_try_cast_params(unsafe { &*params });
        if !params.remote_object_id.get_object_id().is_set() {
            let value = StandardResult::new(error_codes::OK(), vec![]);
            return match dll::ffi::write_standard_result(&state.allocator, &value, unsafe {
                &mut *result
            }) {
                Ok(()) => error_codes::OK(),
                Err(error_code) => {
                    unsafe {
                        (*result).error_code = error_code;
                    }
                    error_code
                }
            };
        }
        let value = state.service.try_cast(params);

        match dll::ffi::write_standard_result(&state.allocator, &value, unsafe { &mut *result }) {
            Ok(()) => error_codes::OK(),
            Err(error_code) => {
                unsafe {
                    (*result).error_code = error_code;
                }
                error_code
            }
        }
    }

    unsafe extern "C" fn parent_add_ref(
        parent_ctx: *mut c_void,
        params: *const dll::ffi::CanopyAddRefParams,
        result: *mut dll::ffi::CanopyStandardResult,
    ) -> i32 {
        if parent_ctx.is_null() || params.is_null() || result.is_null() {
            return error_codes::INVALID_DATA();
        }

        let state = unsafe { &mut *(parent_ctx as *mut ParentCallbackState) };
        let params = dll::ffi::copy_add_ref_params(unsafe { &*params });
        if !params.remote_object_id.get_object_id().is_set() {
            let value = StandardResult::new(error_codes::OK(), vec![]);
            return match dll::ffi::write_standard_result(&state.allocator, &value, unsafe {
                &mut *result
            }) {
                Ok(()) => error_codes::OK(),
                Err(error_code) => {
                    unsafe {
                        (*result).error_code = error_code;
                    }
                    error_code
                }
            };
        }
        let mut value = state.service.add_ref(params);
        if value.error_code == error_codes::OBJECT_NOT_FOUND()
            || value.error_code == error_codes::ZONE_NOT_FOUND()
        {
            value.error_code = error_codes::OK();
        }

        match dll::ffi::write_standard_result(&state.allocator, &value, unsafe { &mut *result }) {
            Ok(()) => error_codes::OK(),
            Err(error_code) => {
                unsafe {
                    (*result).error_code = error_code;
                }
                error_code
            }
        }
    }

    unsafe extern "C" fn parent_release(
        parent_ctx: *mut c_void,
        params: *const dll::ffi::CanopyReleaseParams,
        result: *mut dll::ffi::CanopyStandardResult,
    ) -> i32 {
        if parent_ctx.is_null() || params.is_null() || result.is_null() {
            return error_codes::INVALID_DATA();
        }

        let state = unsafe { &mut *(parent_ctx as *mut ParentCallbackState) };
        let params = dll::ffi::copy_release_params(unsafe { &*params });
        if !params.remote_object_id.get_object_id().is_set() {
            let value = StandardResult::new(error_codes::OK(), vec![]);
            return match dll::ffi::write_standard_result(&state.allocator, &value, unsafe {
                &mut *result
            }) {
                Ok(()) => error_codes::OK(),
                Err(error_code) => {
                    unsafe {
                        (*result).error_code = error_code;
                    }
                    error_code
                }
            };
        }
        let value = state.service.release(params);

        match dll::ffi::write_standard_result(&state.allocator, &value, unsafe { &mut *result }) {
            Ok(()) => error_codes::OK(),
            Err(error_code) => {
                unsafe {
                    (*result).error_code = error_code;
                }
                error_code
            }
        }
    }

    unsafe extern "C" fn parent_post(
        _parent_ctx: *mut c_void,
        _params: *const dll::ffi::CanopyPostParams,
    ) {
    }

    unsafe extern "C" fn parent_object_released(
        _parent_ctx: *mut c_void,
        _params: *const dll::ffi::CanopyObjectReleasedParams,
    ) {
    }

    unsafe extern "C" fn parent_transport_down(
        _parent_ctx: *mut c_void,
        _params: *const dll::ffi::CanopyTransportDownParams,
    ) {
    }

    unsafe extern "C" fn parent_get_new_zone_id(
        parent_ctx: *mut c_void,
        _params: *const dll::ffi::CanopyGetNewZoneIdParams,
        result: *mut dll::ffi::CanopyNewZoneIdResult,
    ) -> i32 {
        if parent_ctx.is_null() || result.is_null() {
            return error_codes::INVALID_DATA();
        }

        let state = unsafe { &mut *(parent_ctx as *mut ParentCallbackState) };
        state.next_zone_subnet += 1;
        let zone = sample_zone(state.next_zone_subnet);
        let result = unsafe { &mut *result };
        let value = NewZoneIdResult::new(error_codes::OK(), zone, Vec::new());

        match dll::ffi::write_new_zone_id_result(&state.allocator, &value, result) {
            Ok(()) => error_codes::OK(),
            Err(error_code) => {
                result.error_code = error_code;
                error_code
            }
        }
    }

    fn sample_zone(subnet: u64) -> Zone {
        Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Ipv4,
                9001,
                vec![127, 0, 0, 1],
                32,
                subnet,
                16,
                0,
                vec![],
            ))
            .expect("sample zone should be valid"),
        )
    }

    // IMarshaller wrapper that forwards send() to the loaded C++ DLL child.
    struct DllMarshaller {
        child: dll::loader::LoadedChild,
        last_add_ref: Arc<Mutex<Option<AddRefParams>>>,
        last_release: Arc<Mutex<Option<ReleaseParams>>>,
        forward_add_ref: bool,
        forward_release: bool,
    }

    // SAFETY: single-threaded tests only; LoadedChild raw pointers do not move.
    unsafe impl Send for DllMarshaller {}
    unsafe impl Sync for DllMarshaller {}

    impl IMarshaller for DllMarshaller {
        fn send(&self, params: SendParams) -> SendResult {
            self.child.send(&params)
        }
        fn post(&self, _params: PostParams) {}
        fn try_cast(&self, _params: TryCastParams) -> StandardResult {
            StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![])
        }
        fn add_ref(&self, params: AddRefParams) -> StandardResult {
            *self
                .last_add_ref
                .lock()
                .expect("last_add_ref mutex poisoned") = Some(params.clone());
            if !self.forward_add_ref {
                return StandardResult::new(error_codes::OK(), vec![]);
            }
            self.child.add_ref(&params)
        }
        fn release(&self, params: ReleaseParams) -> StandardResult {
            *self
                .last_release
                .lock()
                .expect("last_release mutex poisoned") = Some(params.clone());
            if !self.forward_release {
                return StandardResult::new(error_codes::OK(), vec![]);
            }
            self.child.release(&params)
        }
        fn object_released(&self, _params: ObjectReleasedParams) {}
        fn transport_down(&self, _params: TransportDownParams) {}
        fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
            NewZoneIdResult::new(error_codes::ZONE_NOT_FOUND(), Zone::default(), vec![])
        }
    }

    struct CxxProbeRuntime {
        _alloc_state: Box<AllocState>,
        _parent_state: Box<ParentCallbackState>,
        proxy: i_math::ProxySkeleton,
        last_add_ref: Arc<Mutex<Option<AddRefParams>>>,
        last_release: Arc<Mutex<Option<ReleaseParams>>>,
    }

    fn create_cxx_probe_runtime(
        parent_subnet: u64,
        child_subnet: u64,
        forward_add_ref: bool,
    ) -> Option<CxxProbeRuntime> {
        let Ok(path) = std::env::var("CANOPY_CXX_PROBE_DLL_PATH") else {
            return None;
        };

        let parent_zone = sample_zone(parent_subnet);
        let child_zone = sample_zone(child_subnet);
        let parent_service = Arc::new(canopy_rpc::Service::new("rust-parent", parent_zone.clone()));
        let mut alloc_state = Box::new(AllocState {
            allocations: HashMap::new(),
        });
        let allocator = CanopyAllocatorVtable {
            allocator_ctx: (&mut *alloc_state as *mut AllocState).cast(),
            alloc: Some(test_alloc),
            free: Some(test_free),
        };
        let mut parent_state = Box::new(ParentCallbackState {
            next_zone_subnet: 1000,
            allocator,
            service: parent_service.clone(),
        });

        // Zone-only remote_object_id: object_id == 0 so C++ skips the i_peer parent bind.
        let input_descr = CanopyConnectionSettings {
            inbound_interface_id: i_peer::ID_RPC_V3,
            outbound_interface_id: i_math::ID_RPC_V3,
            remote_object_id: CanopyRemoteObject {
                address: dll::ffi::borrow_zone(&parent_zone).address,
            },
        };

        let mut init_params = CanopyDllInitParams {
            name: c"rust-parent".as_ptr(),
            parent_zone: dll::ffi::borrow_zone(&parent_zone),
            child_zone: dll::ffi::borrow_zone(&child_zone),
            input_descr: &input_descr,
            parent_ctx: (&mut *parent_state as *mut ParentCallbackState).cast(),
            allocator,
            parent_send: Some(parent_send),
            parent_post: Some(parent_post),
            parent_try_cast: Some(parent_try_cast),
            parent_add_ref: Some(parent_add_ref),
            parent_release: Some(parent_release),
            parent_object_released: Some(parent_object_released),
            parent_transport_down: Some(parent_transport_down),
            parent_get_new_zone_id: Some(parent_get_new_zone_id),
            ..CanopyDllInitParams::default()
        };

        let library = dll::loader::DynamicLibrary::load(&path).expect("C++ probe DLL should load");
        let child = library
            .init_child(&mut init_params)
            .expect("C++ probe DLL should init");

        let output_obj = child.output_obj().clone();
        let caller_zone = parent_zone;
        let last_add_ref = Arc::new(Mutex::new(None));
        let last_release = Arc::new(Mutex::new(None));
        let marshaller = Arc::new(DllMarshaller {
            child,
            last_add_ref: last_add_ref.clone(),
            last_release: last_release.clone(),
            forward_add_ref,
            forward_release: forward_add_ref,
        });
        let transport = canopy_rpc::Transport::new(
            "rust-parent-to-cxx-child",
            parent_service.zone_id(),
            child_zone.clone(),
            Arc::downgrade(&parent_service),
            marshaller.clone(),
        );
        transport.set_status(canopy_rpc::TransportStatus::Connected);
        parent_service.add_transport(child_zone, transport.clone());
        let service_proxy = canopy_rpc::ServiceProxy::with_transport(
            parent_service.clone(),
            transport,
            canopy_rpc::GeneratedRpcCallContext {
                protocol_version: DefaultValues::VERSION_3 as u64,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 0,
                caller_zone_id: caller_zone,
                remote_object_id: output_obj,
            },
        );
        let proxy = i_math::ProxySkeleton::with_caller(service_proxy);

        Some(CxxProbeRuntime {
            _alloc_state: alloc_state,
            _parent_state: parent_state,
            proxy,
            last_add_ref,
            last_release,
        })
    }

    #[test]
    fn rust_proxy_can_call_cxx_dll_protobuf_method() {
        let _guard = CXX_DLL_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let Some(runtime) = create_cxx_probe_runtime(55, 56, true) else {
            return;
        };
        let proxy = runtime.proxy;

        // Scalar: add(20, 22) -> c == 42
        let mut c = 0i32;
        assert_eq!(proxy.add(20, 22, &mut c), canopy_rpc::OK());
        assert_eq!(c, 42);

        // String: bounce_text("hello") -> "cxx-probe:hello"
        let mut out = String::new();
        assert_eq!(
            proxy.bounce_text("hello".to_string(), &mut out),
            canopy_rpc::OK()
        );
        assert_eq!(out, "cxx-probe:hello");

        // Vec<u64>: reverse_sequence([1,2,3]) -> [3,2,1]
        let mut rev = vec![];
        assert_eq!(
            proxy.reverse_sequence(vec![1, 2, 3], &mut rev),
            canopy_rpc::OK()
        );
        assert_eq!(rev, vec![3u64, 2, 1]);

        let peer = Arc::new(super::tests::PeerImpl);
        let mut seen_shared = -1;
        assert_eq!(
            proxy.accept_shared_peer(
                canopy_rpc::Shared::from_value(peer.clone()),
                &mut seen_shared
            ),
            canopy_rpc::OK()
        );
        assert_eq!(seen_shared, 307);

        let mut seen_optimistic = -1;
        assert_eq!(
            proxy.accept_optimistic_peer(
                canopy_rpc::Optimistic::from_value(canopy_rpc::LocalProxy::from_shared(&peer)),
                &mut seen_optimistic
            ),
            canopy_rpc::OK()
        );
        assert_eq!(seen_optimistic, 407);

        let mut created_shared: canopy_rpc::Shared<Arc<i_peer::ProxySkeleton>> =
            canopy_rpc::Shared::null();
        assert_eq!(
            proxy.create_shared_peer(&mut created_shared),
            canopy_rpc::OK()
        );
        let canopy_rpc::BoundInterface::Value(created_shared_peer) = created_shared.into_inner()
        else {
            panic!("expected C++ created shared peer");
        };
        let mut created_shared_ping = 0;
        assert_eq!(
            created_shared_peer.ping(&mut created_shared_ping),
            canopy_rpc::OK()
        );
        assert_eq!(created_shared_ping, 11);

        let mut created_optimistic: canopy_rpc::Optimistic<
            canopy_rpc::LocalProxy<i_peer::ProxySkeleton>,
        > = canopy_rpc::Optimistic::null();
        assert_eq!(
            proxy.create_optimistic_peer(&mut created_optimistic),
            canopy_rpc::OK()
        );
        let canopy_rpc::BoundInterface::Value(created_optimistic_peer) =
            created_optimistic.into_inner()
        else {
            panic!("expected C++ created optimistic peer");
        };
        let created_optimistic_peer = created_optimistic_peer
            .upgrade()
            .expect("remote optimistic proxy should upgrade");
        let mut created_optimistic_ping = 0;
        assert_eq!(
            created_optimistic_peer.ping(&mut created_optimistic_ping),
            canopy_rpc::OK()
        );
        assert_eq!(created_optimistic_ping, 11);

        let mut echoed_shared: canopy_rpc::Shared<Arc<super::tests::PeerImpl>> =
            canopy_rpc::Shared::null();
        assert_eq!(
            proxy.echo_shared_peer(
                canopy_rpc::Shared::from_value(peer.clone()),
                &mut echoed_shared
            ),
            canopy_rpc::OK()
        );
        let canopy_rpc::BoundInterface::Value(echoed_shared_peer) = echoed_shared.into_inner()
        else {
            panic!("expected echoed shared peer");
        };
        let mut echoed_shared_ping = 0;
        assert_eq!(
            echoed_shared_peer.ping(&mut echoed_shared_ping),
            canopy_rpc::OK()
        );
        assert_eq!(echoed_shared_ping, 7);

        let mut echoed_optimistic: canopy_rpc::Optimistic<
            canopy_rpc::LocalProxy<super::tests::PeerImpl>,
        > = canopy_rpc::Optimistic::null();
        assert_eq!(
            proxy.echo_optimistic_peer(
                canopy_rpc::Optimistic::from_value(canopy_rpc::LocalProxy::from_shared(&peer)),
                &mut echoed_optimistic
            ),
            canopy_rpc::OK()
        );
        let canopy_rpc::BoundInterface::Value(echoed_optimistic_peer) =
            echoed_optimistic.into_inner()
        else {
            panic!("expected echoed optimistic peer");
        };
        let echoed_optimistic_peer = echoed_optimistic_peer
            .upgrade()
            .expect("remote optimistic proxy should upgrade");
        let mut echoed_optimistic_ping = 0;
        assert_eq!(
            echoed_optimistic_peer.ping(&mut echoed_optimistic_ping),
            canopy_rpc::OK()
        );
        assert_eq!(echoed_optimistic_ping, 7);
    }

    include!("../../../../integration_tests/rust_cxx_y_topology/requesting_zone_add_ref.rs");
}
