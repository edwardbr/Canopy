use canopy_protobuf_runtime_probe::fuzz_test::fuzz_test::{
    self as fg, IAutonomousNode, ICleanup, IFuzzCache, IFuzzFactory, IFuzzWorker,
    IGarbageCollector, ISharedObject, Instruction, NodeType,
};
use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddressType, ChildService, DefaultValues, GetNewZoneIdParams, InterfacePointerKind, Object,
    RemoteObject, SendParams, SendResult, ServiceRuntime, Zone, ZoneAddress, ZoneAddressArgs,
};
use canopy_transport_dynamic_library as dll;
use canopy_transport_local::create_child_zone_with_exported_object;
use std::sync::{Arc, Mutex};

fn child_output_object(params: &dll::CanopyDllInitParams) -> RemoteObject {
    let child_zone = dll::decode_zone_or_else(params.child_zone, || {
        Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Ipv4,
                0,
                vec![127, 0, 0, 1],
                32,
                77,
                16,
                0,
                vec![],
            ))
            .expect("fallback child zone"),
        )
    });
    child_zone
        .with_object(Object::new(100))
        .expect("child output object")
}

fn local_child_zone(child_zone_id: u64) -> Zone {
    Zone::new(
        ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Local,
            0,
            vec![],
            DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
            child_zone_id,
            DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
            0,
            vec![],
        ))
        .expect("local child zone"),
    )
}

#[derive(Clone)]
struct RustFuzzNode {
    state: Arc<Mutex<RustFuzzNodeState>>,
    service_runtime: Option<Arc<dyn ServiceRuntime>>,
    object_id: Option<Object>,
}

struct RustFuzzNodeState {
    node_type: NodeType,
    node_id: u64,
    signals_received: i32,
    connections: Vec<canopy_rpc::SharedPtr<dyn IAutonomousNode>>,
    child_nodes: Vec<canopy_rpc::SharedPtr<dyn IAutonomousNode>>,
    created_objects: Vec<canopy_rpc::SharedPtr<dyn ISharedObject>>,
    child_zones: Vec<canopy_transport_local::BoundChildZone>,
    parent_node: canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    local_factory: canopy_rpc::SharedPtr<dyn IFuzzFactory>,
    local_cache: canopy_rpc::SharedPtr<dyn IFuzzCache>,
    local_worker: canopy_rpc::SharedPtr<dyn IFuzzWorker>,
    cleanup_called: bool,
}

impl Default for RustFuzzNode {
    fn default() -> Self {
        Self {
            state: Arc::new(Mutex::new(RustFuzzNodeState {
                node_type: NodeType::RootNode,
                node_id: 0,
                signals_received: 0,
                connections: Vec::new(),
                child_nodes: Vec::new(),
                created_objects: Vec::new(),
                child_zones: Vec::new(),
                parent_node: canopy_rpc::Shared::null(),
                local_factory: canopy_rpc::Shared::null(),
                local_cache: canopy_rpc::Shared::null(),
                local_worker: canopy_rpc::Shared::null(),
                cleanup_called: false,
            })),
            service_runtime: None,
            object_id: None,
        }
    }
}

impl RustFuzzNode {
    fn with_service_and_object(
        service_runtime: Arc<dyn ServiceRuntime>,
        object_id: Object,
    ) -> Self {
        Self {
            service_runtime: Some(service_runtime),
            object_id: Some(object_id),
            ..Self::default()
        }
    }

    fn local_node_ptr(&self) -> canopy_rpc::SharedPtr<dyn IAutonomousNode> {
        let (Some(service_runtime), Some(object_id)) = (&self.service_runtime, self.object_id)
        else {
            let node: Arc<dyn IAutonomousNode> = Arc::new(self.clone());
            return canopy_rpc::Shared::from_arc(node);
        };

        canopy_rpc::get_interface_view::<dyn IAutonomousNode>(
            service_runtime.as_ref(),
            object_id,
            canopy_rpc::InterfaceOrdinal::new(fg::i_autonomous_node::ID_RPC_V3),
        )
        .map(canopy_rpc::Shared::from_arc)
        .unwrap_or_else(|_| {
            let node: Arc<dyn IAutonomousNode> = Arc::new(self.clone());
            canopy_rpc::Shared::from_arc(node)
        })
    }

    fn shared_object_ptr(&self, value: i32) -> canopy_rpc::SharedPtr<dyn ISharedObject> {
        let object = RustSharedObject::default();
        let _ = object.set_value(value);
        if let Some(service_runtime) = &self.service_runtime {
            let rpc_object = fg::i_shared_object::make_rpc_object(object.clone());
            let object_id = service_runtime.generate_new_object_id();
            if service_runtime
                .register_rpc_object(object_id, rpc_object)
                .is_ok()
            {
                if let Ok(view) = canopy_rpc::get_interface_view::<dyn ISharedObject>(
                    service_runtime.as_ref(),
                    object_id,
                    canopy_rpc::InterfaceOrdinal::new(fg::i_shared_object::ID_RPC_V3),
                ) {
                    return canopy_rpc::Shared::from_arc(view);
                }
            }
        }

        let object: Arc<dyn ISharedObject> = Arc::new(object);
        canopy_rpc::Shared::from_arc(object)
    }

    fn autonomous_node_ptr(
        &self,
        node_type: NodeType,
        node_id: u64,
        parent_node: canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> Result<canopy_rpc::SharedPtr<dyn IAutonomousNode>, i32> {
        let Some(parent_runtime) = &self.service_runtime else {
            let child: Arc<dyn IAutonomousNode> = Arc::new(RustFuzzNode {
                state: Arc::new(Mutex::new(RustFuzzNodeState {
                    node_type,
                    node_id,
                    signals_received: 0,
                    connections: Vec::new(),
                    child_nodes: Vec::new(),
                    created_objects: Vec::new(),
                    child_zones: Vec::new(),
                    parent_node,
                    local_factory: canopy_rpc::Shared::null(),
                    local_cache: canopy_rpc::Shared::null(),
                    local_worker: canopy_rpc::Shared::null(),
                    cleanup_called: false,
                })),
                service_runtime: None,
                object_id: None,
            });
            return Ok(canopy_rpc::Shared::from_arc(child));
        };
        let child = RustFuzzNode {
            state: Arc::new(Mutex::new(RustFuzzNodeState {
                node_type,
                node_id,
                signals_received: 0,
                connections: Vec::new(),
                child_nodes: Vec::new(),
                created_objects: Vec::new(),
                child_zones: Vec::new(),
                parent_node,
                local_factory: canopy_rpc::Shared::null(),
                local_cache: canopy_rpc::Shared::null(),
                local_worker: canopy_rpc::Shared::null(),
                cleanup_called: false,
            })),
            service_runtime: None,
            object_id: None,
        };
        let bound_zone = create_child_zone_with_exported_object(
            format!("rust-fuzz-local-child-{node_id}"),
            parent_runtime.clone(),
            format!("rust-fuzz-child-{node_id}"),
            local_child_zone(node_id),
            |child_service, object_id| {
                let child = RustFuzzNode {
                    service_runtime: Some(child_service),
                    object_id: Some(object_id),
                    ..child
                };
                make_fuzz_node_rpc_object(child)
            },
            InterfacePointerKind::Shared,
        )?;
        let remote_object = bound_zone.root_object().clone();

        let remote_interface = canopy_rpc::create_remote_shared_interface(
            parent_runtime.as_ref(),
            remote_object,
            fg::i_autonomous_node::create_remote_shared,
        )?;
        self.state
            .lock()
            .expect("node state mutex poisoned")
            .child_zones
            .push(bound_zone);
        Ok(remote_interface)
    }

    fn allocate_child_zone(&self) -> Result<Zone, i32> {
        let Some(service_runtime) = &self.service_runtime else {
            return Err(error_codes::ZONE_NOT_INITIALISED());
        };
        let result = service_runtime.get_new_zone_id(GetNewZoneIdParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            in_back_channel: Vec::new(),
        });
        if !error_codes::is_error(result.error_code) && result.zone_id.is_set() {
            return Ok(result.zone_id);
        }
        let state = self.state.lock().expect("node state mutex poisoned");
        Ok(local_child_zone(
            state.node_id.saturating_mul(1000) + state.child_zones.len() as u64 + 1,
        ))
    }

    fn create_local_factory(&self) -> i32 {
        {
            let state = self.state.lock().expect("node state mutex poisoned");
            if state.local_factory.as_ref().is_some() {
                return error_codes::OK();
            }
        }
        let Some(parent_runtime) = &self.service_runtime else {
            return error_codes::ZONE_NOT_INITIALISED();
        };
        let child_zone = match self.allocate_child_zone() {
            Ok(zone) => zone,
            Err(error_code) => return error_code,
        };
        let bound_zone = match create_child_zone_with_exported_object(
            format!(
                "rust-fuzz-factory-parent-{}",
                self.state
                    .lock()
                    .expect("node state mutex poisoned")
                    .node_id
            ),
            parent_runtime.clone(),
            "rust-fuzz-factory",
            child_zone,
            |child_service, _object_id| {
                fg::i_fuzz_factory::make_rpc_object(RustFuzzFactory {
                    objects_created: Arc::new(Mutex::new(0)),
                    service_runtime: Some(child_service),
                })
            },
            InterfacePointerKind::Shared,
        ) {
            Ok(zone) => zone,
            Err(error_code) => return error_code,
        };
        let remote_interface = match canopy_rpc::create_remote_shared_interface(
            parent_runtime.as_ref(),
            bound_zone.root_object().clone(),
            fg::i_fuzz_factory::create_remote_shared,
        ) {
            Ok(value) => value,
            Err(error_code) => return error_code,
        };
        let mut state = self.state.lock().expect("node state mutex poisoned");
        state.local_factory = remote_interface;
        state.child_zones.push(bound_zone);
        error_codes::OK()
    }

    fn create_local_cache(&self) -> i32 {
        {
            let state = self.state.lock().expect("node state mutex poisoned");
            if state.local_cache.as_ref().is_some() {
                return error_codes::OK();
            }
        }
        let Some(parent_runtime) = &self.service_runtime else {
            return error_codes::ZONE_NOT_INITIALISED();
        };
        let child_zone = match self.allocate_child_zone() {
            Ok(zone) => zone,
            Err(error_code) => return error_code,
        };
        let bound_zone = match create_child_zone_with_exported_object(
            format!(
                "rust-fuzz-cache-parent-{}",
                self.state
                    .lock()
                    .expect("node state mutex poisoned")
                    .node_id
            ),
            parent_runtime.clone(),
            "rust-fuzz-cache",
            child_zone,
            |_child_service, _object_id| fg::i_fuzz_cache::make_rpc_object(RustFuzzCache::default()),
            InterfacePointerKind::Shared,
        ) {
            Ok(zone) => zone,
            Err(error_code) => return error_code,
        };
        let remote_interface = match canopy_rpc::create_remote_shared_interface(
            parent_runtime.as_ref(),
            bound_zone.root_object().clone(),
            fg::i_fuzz_cache::create_remote_shared,
        ) {
            Ok(value) => value,
            Err(error_code) => return error_code,
        };
        let mut state = self.state.lock().expect("node state mutex poisoned");
        state.local_cache = remote_interface;
        state.child_zones.push(bound_zone);
        error_codes::OK()
    }

    fn create_local_worker(&self) -> i32 {
        {
            let state = self.state.lock().expect("node state mutex poisoned");
            if state.local_worker.as_ref().is_some() {
                return error_codes::OK();
            }
        }
        let Some(parent_runtime) = &self.service_runtime else {
            return error_codes::ZONE_NOT_INITIALISED();
        };
        let child_zone = match self.allocate_child_zone() {
            Ok(zone) => zone,
            Err(error_code) => return error_code,
        };
        let bound_zone = match create_child_zone_with_exported_object(
            format!(
                "rust-fuzz-worker-parent-{}",
                self.state
                    .lock()
                    .expect("node state mutex poisoned")
                    .node_id
            ),
            parent_runtime.clone(),
            "rust-fuzz-worker",
            child_zone,
            |_child_service, _object_id| {
                fg::i_fuzz_worker::make_rpc_object(RustFuzzWorker::default())
            },
            InterfacePointerKind::Shared,
        ) {
            Ok(zone) => zone,
            Err(error_code) => return error_code,
        };
        let remote_interface = match canopy_rpc::create_remote_shared_interface(
            parent_runtime.as_ref(),
            bound_zone.root_object().clone(),
            fg::i_fuzz_worker::create_remote_shared,
        ) {
            Ok(value) => value,
            Err(error_code) => return error_code,
        };
        let mut state = self.state.lock().expect("node state mutex poisoned");
        state.local_worker = remote_interface;
        state.child_zones.push(bound_zone);
        error_codes::OK()
    }
}

impl canopy_rpc::internal::CreateLocalProxy for RustFuzzNode {}

impl canopy_rpc::CastingInterface for RustFuzzNode {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_autonomous_node::matches_interface_id(interface_id)
            || fg::i_cleanup::matches_interface_id(interface_id)
    }
}

struct RustFuzzNodeAdapter;

impl canopy_rpc::LocalObjectAdapter<RustFuzzNode> for RustFuzzNodeAdapter {
    fn interface_name() -> &'static str {
        fg::i_autonomous_node::NAME
    }

    fn get_id(rpc_version: u64) -> u64 {
        if rpc_version >= 3 {
            fg::i_autonomous_node::ID_RPC_V3
        } else {
            fg::i_autonomous_node::ID_RPC_V2
        }
    }

    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        fg::i_autonomous_node::interface_binding::METHODS
    }

    fn supports_interface(interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_autonomous_node::matches_interface_id(interface_id)
            || fg::i_cleanup::matches_interface_id(interface_id)
    }

    fn dispatch(
        implementation: &RustFuzzNode,
        context: &canopy_rpc::DispatchContext,
        params: SendParams,
    ) -> SendResult {
        if fg::i_autonomous_node::matches_interface_id(params.interface_id) {
            return IAutonomousNode::__rpc_dispatch_generated(implementation, context, params);
        }
        if fg::i_cleanup::matches_interface_id(params.interface_id) {
            return ICleanup::__rpc_dispatch_generated(implementation, context, params);
        }
        SendResult::new(error_codes::INVALID_INTERFACE_ID(), Vec::new(), Vec::new())
    }

    fn method_metadata(
        interface_id: canopy_rpc::InterfaceOrdinal,
    ) -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        if fg::i_autonomous_node::matches_interface_id(interface_id) {
            return fg::i_autonomous_node::interface_binding::METHODS;
        }
        if fg::i_cleanup::matches_interface_id(interface_id) {
            return fg::i_cleanup::interface_binding::METHODS;
        }
        &[]
    }

    fn local_interface_view(
        object: Arc<canopy_rpc::RpcBase<RustFuzzNode, Self>>,
        interface_id: canopy_rpc::InterfaceOrdinal,
    ) -> Option<Arc<dyn std::any::Any + Send + Sync>> {
        if fg::i_autonomous_node::matches_interface_id(interface_id) {
            let view: Arc<dyn IAutonomousNode> = Arc::new(RustFuzzAutonomousNodeView { object });
            return Some(Arc::new(canopy_rpc::internal::LocalInterfaceView::new(
                view,
            )));
        }
        if fg::i_cleanup::matches_interface_id(interface_id) {
            let view: Arc<dyn ICleanup> = Arc::new(RustFuzzCleanupView { object });
            return Some(Arc::new(canopy_rpc::internal::LocalInterfaceView::new(
                view,
            )));
        }
        None
    }
}

struct RustFuzzAutonomousNodeView {
    object: Arc<canopy_rpc::RpcBase<RustFuzzNode, RustFuzzNodeAdapter>>,
}

impl canopy_rpc::CastingInterface for RustFuzzAutonomousNodeView {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_autonomous_node::matches_interface_id(interface_id)
    }

    fn __rpc_call(&self, params: SendParams) -> SendResult {
        canopy_rpc::CastingInterface::__rpc_call(self.object.as_ref(), params)
    }

    fn __rpc_local_object_stub(&self) -> Option<Arc<Mutex<canopy_rpc::internal::ObjectStub>>> {
        canopy_rpc::internal::get_object_stub(self.object.as_ref())
    }
}

impl IAutonomousNode for RustFuzzAutonomousNodeView {
    fn initialize_node(&self, r#type: NodeType, node_id: u64) -> i32 {
        self.object
            .implementation()
            .initialize_node(r#type, node_id)
    }

    fn run_script(
        &self,
        target_node: canopy_rpc::SharedPtr<dyn IAutonomousNode>,
        instruction_count: i32,
    ) -> i32 {
        self.object
            .implementation()
            .run_script(target_node, instruction_count)
    }

    fn execute_instruction(
        &self,
        instruction: Instruction,
        input_object: canopy_rpc::SharedPtr<dyn ISharedObject>,
        output_object: &mut canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        self.object
            .implementation()
            .execute_instruction(instruction, input_object, output_object)
    }

    fn connect_to_node(&self, target_node: canopy_rpc::SharedPtr<dyn IAutonomousNode>) -> i32 {
        self.object.implementation().connect_to_node(target_node)
    }

    fn pass_object_to_connected(
        &self,
        connection_index: i32,
        object: canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        self.object
            .implementation()
            .pass_object_to_connected(connection_index, object)
    }

    fn receive_object(
        &self,
        object: canopy_rpc::SharedPtr<dyn ISharedObject>,
        sender_node_id: u64,
    ) -> i32 {
        self.object
            .implementation()
            .receive_object(object, sender_node_id)
    }

    fn get_node_status(
        &self,
        current_type: &mut NodeType,
        current_id: &mut u64,
        connections_count: &mut i32,
        objects_held: &mut i32,
    ) -> i32 {
        self.object.implementation().get_node_status(
            current_type,
            current_id,
            connections_count,
            objects_held,
        )
    }

    fn create_child_node(
        &self,
        child_type: NodeType,
        child_zone_id: u64,
        cache_locally: bool,
        child_node: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> i32 {
        self.object.implementation().create_child_node(
            child_type,
            child_zone_id,
            cache_locally,
            child_node,
        )
    }

    fn request_child_creation(
        &self,
        target_parent: canopy_rpc::SharedPtr<dyn IAutonomousNode>,
        child_type: NodeType,
        child_zone_id: u64,
        child_proxy: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> i32 {
        self.object.implementation().request_child_creation(
            target_parent,
            child_type,
            child_zone_id,
            child_proxy,
        )
    }

    fn get_cached_children_count(&self, count: &mut i32) -> i32 {
        self.object
            .implementation()
            .get_cached_children_count(count)
    }

    fn get_cached_child_by_index(
        &self,
        index: i32,
        child: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> i32 {
        self.object
            .implementation()
            .get_cached_child_by_index(index, child)
    }

    fn get_parent_node(&self, parent: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>) -> i32 {
        self.object.implementation().get_parent_node(parent)
    }

    fn set_parent_node(&self, parent: canopy_rpc::SharedPtr<dyn IAutonomousNode>) -> i32 {
        self.object.implementation().set_parent_node(parent)
    }
}

struct RustFuzzCleanupView {
    object: Arc<canopy_rpc::RpcBase<RustFuzzNode, RustFuzzNodeAdapter>>,
}

impl canopy_rpc::CastingInterface for RustFuzzCleanupView {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_cleanup::matches_interface_id(interface_id)
    }

    fn __rpc_call(&self, params: SendParams) -> SendResult {
        canopy_rpc::CastingInterface::__rpc_call(self.object.as_ref(), params)
    }

    fn __rpc_local_object_stub(&self) -> Option<Arc<Mutex<canopy_rpc::internal::ObjectStub>>> {
        canopy_rpc::internal::get_object_stub(self.object.as_ref())
    }
}

impl ICleanup for RustFuzzCleanupView {
    fn cleanup(&self, collector: canopy_rpc::SharedPtr<dyn IGarbageCollector>) -> i32 {
        self.object.implementation().cleanup(collector)
    }
}

fn make_fuzz_node_rpc_object(
    node: RustFuzzNode,
) -> Arc<canopy_rpc::RpcBase<RustFuzzNode, RustFuzzNodeAdapter>> {
    canopy_rpc::make_rpc_object_with_adapter::<RustFuzzNode, RustFuzzNodeAdapter>(node)
}

#[derive(Clone, Default)]
#[allow(dead_code)]
struct RustSharedObject {
    state: Arc<Mutex<RustSharedObjectState>>,
}

#[derive(Default)]
struct RustSharedObjectState {
    value: i32,
    test_count: i32,
}

impl canopy_rpc::internal::CreateLocalProxy for RustSharedObject {}

impl canopy_rpc::CastingInterface for RustSharedObject {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_shared_object::matches_interface_id(interface_id)
    }
}

impl ISharedObject for RustSharedObject {
    fn test_function(&self, input_value: i32) -> i32 {
        let mut state = self
            .state
            .lock()
            .expect("shared object state mutex poisoned");
        state.test_count += 1;
        input_value * 2 + state.test_count
    }

    fn get_stats(&self, count: &mut i32) -> i32 {
        *count = self
            .state
            .lock()
            .expect("shared object state mutex poisoned")
            .test_count;
        error_codes::OK()
    }

    fn set_value(&self, new_value: i32) -> i32 {
        self.state
            .lock()
            .expect("shared object state mutex poisoned")
            .value = new_value;
        error_codes::OK()
    }

    fn get_value(&self, value: &mut i32) -> i32 {
        *value = self
            .state
            .lock()
            .expect("shared object state mutex poisoned")
            .value;
        error_codes::OK()
    }
}

#[derive(Clone, Default)]
struct RustFuzzFactory {
    objects_created: Arc<Mutex<i32>>,
    service_runtime: Option<Arc<dyn ServiceRuntime>>,
}

impl canopy_rpc::internal::CreateLocalProxy for RustFuzzFactory {}

impl canopy_rpc::CastingInterface for RustFuzzFactory {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_fuzz_factory::matches_interface_id(interface_id)
    }
}

impl IFuzzFactory for RustFuzzFactory {
    fn create_shared_object(
        &self,
        id: i32,
        _name: String,
        initial_value: i32,
        created_object: &mut canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        let object = RustSharedObject::default();
        let _ = object.set_value(initial_value);
        if let Some(service_runtime) = &self.service_runtime {
            let rpc_object = fg::i_shared_object::make_rpc_object(object.clone());
            let object_id = service_runtime.generate_new_object_id();
            if service_runtime
                .register_rpc_object(object_id, rpc_object)
                .is_ok()
                && let Ok(view) = canopy_rpc::get_interface_view::<dyn ISharedObject>(
                    service_runtime.as_ref(),
                    object_id,
                    canopy_rpc::InterfaceOrdinal::new(fg::i_shared_object::ID_RPC_V3),
                )
            {
                *created_object = canopy_rpc::Shared::from_arc(view);
            } else {
                return error_codes::INVALID_DATA();
            }
        } else {
            let shared: Arc<dyn ISharedObject> = Arc::new(object);
            *created_object = canopy_rpc::Shared::from_arc(shared);
        }
        *self
            .objects_created
            .lock()
            .expect("factory created count mutex poisoned") += 1;
        let _ = id;
        error_codes::OK()
    }

    fn place_shared_object(
        &self,
        new_object: canopy_rpc::SharedPtr<dyn ISharedObject>,
        target_object: canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        let Some(new_object) = new_object.as_ref() else {
            return error_codes::INVALID_DATA();
        };
        let Some(target_object) = target_object.as_ref() else {
            return error_codes::INVALID_DATA();
        };
        let mut new_value = 0;
        let mut target_value = 0;
        let new_result = new_object.get_value(&mut new_value);
        let target_result = target_object.get_value(&mut target_value);
        if new_result != error_codes::OK() || target_result != error_codes::OK() {
            return error_codes::INVALID_DATA();
        }
        target_object.set_value(new_value + target_value)
    }

    fn get_factory_stats(&self, total_created: &mut i32, current_refs: &mut i32) -> i32 {
        *total_created = *self
            .objects_created
            .lock()
            .expect("factory created count mutex poisoned");
        *current_refs = 0;
        error_codes::OK()
    }
}

#[derive(Clone, Default)]
struct RustFuzzCache {
    storage: Arc<Mutex<std::collections::BTreeMap<i32, canopy_rpc::SharedPtr<dyn ISharedObject>>>>,
}

impl canopy_rpc::internal::CreateLocalProxy for RustFuzzCache {}

impl canopy_rpc::CastingInterface for RustFuzzCache {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_fuzz_cache::matches_interface_id(interface_id)
    }
}

impl IFuzzCache for RustFuzzCache {
    fn store_object(
        &self,
        cache_key: i32,
        object: canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        if object.as_ref().is_none() {
            return error_codes::INVALID_DATA();
        }
        self.storage
            .lock()
            .expect("cache storage mutex poisoned")
            .insert(cache_key, object);
        error_codes::OK()
    }

    fn retrieve_object(
        &self,
        cache_key: i32,
        object: &mut canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        let storage = self.storage.lock().expect("cache storage mutex poisoned");
        if let Some(value) = storage.get(&cache_key) {
            *object = value.clone();
            return error_codes::OK();
        }
        *object = canopy_rpc::Shared::null();
        error_codes::OBJECT_NOT_FOUND()
    }

    fn has_object(&self, cache_key: i32, exists: &mut bool) -> i32 {
        *exists = self
            .storage
            .lock()
            .expect("cache storage mutex poisoned")
            .contains_key(&cache_key);
        error_codes::OK()
    }

    fn get_cache_size(&self, size: &mut i32) -> i32 {
        *size = self
            .storage
            .lock()
            .expect("cache storage mutex poisoned")
            .len() as i32;
        error_codes::OK()
    }
}

#[derive(Clone, Default)]
struct RustFuzzWorker {
    objects_processed: Arc<Mutex<i32>>,
    total_increments: Arc<Mutex<i32>>,
}

impl canopy_rpc::internal::CreateLocalProxy for RustFuzzWorker {}

impl canopy_rpc::CastingInterface for RustFuzzWorker {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::i_fuzz_worker::matches_interface_id(interface_id)
    }
}

impl IFuzzWorker for RustFuzzWorker {
    fn process_object(
        &self,
        object: canopy_rpc::SharedPtr<dyn ISharedObject>,
        increment: i32,
    ) -> i32 {
        let Some(object) = object.as_ref() else {
            return error_codes::INVALID_DATA();
        };
        let mut current_value = 0;
        if object.get_value(&mut current_value) != error_codes::OK() {
            return error_codes::INVALID_DATA();
        }
        if object.set_value(current_value + increment) != error_codes::OK() {
            return error_codes::INVALID_DATA();
        }
        *self
            .objects_processed
            .lock()
            .expect("worker processed count mutex poisoned") += 1;
        *self
            .total_increments
            .lock()
            .expect("worker increments mutex poisoned") += increment;
        error_codes::OK()
    }

    fn get_worker_stats(&self, objects_processed: &mut i32, total_increments: &mut i32) -> i32 {
        *objects_processed = *self
            .objects_processed
            .lock()
            .expect("worker processed count mutex poisoned");
        *total_increments = *self
            .total_increments
            .lock()
            .expect("worker increments mutex poisoned");
        error_codes::OK()
    }
}

impl ICleanup for RustFuzzNode {
    fn cleanup(&self, _collector: canopy_rpc::SharedPtr<dyn IGarbageCollector>) -> i32 {
        let mut state = self.state.lock().expect("node state mutex poisoned");
        if state.cleanup_called {
            return error_codes::OK();
        }
        state.cleanup_called = true;
        state.connections.clear();
        state.child_nodes.clear();
        state.created_objects.clear();
        state.child_zones.clear();
        state.parent_node = canopy_rpc::Shared::null();
        state.local_factory = canopy_rpc::Shared::null();
        state.local_cache = canopy_rpc::Shared::null();
        state.local_worker = canopy_rpc::Shared::null();
        error_codes::OK()
    }
}

impl IAutonomousNode for RustFuzzNode {
    fn initialize_node(&self, r#type: NodeType, node_id: u64) -> i32 {
        let mut state = self.state.lock().expect("node state mutex poisoned");
        state.node_type = r#type;
        state.node_id = node_id;
        error_codes::OK()
    }

    fn run_script(
        &self,
        target_node: canopy_rpc::SharedPtr<dyn IAutonomousNode>,
        instruction_count: i32,
    ) -> i32 {
        let Some(target_node) = target_node.as_ref() else {
            return error_codes::INVALID_DATA();
        };
        let sender_node_id = self
            .state
            .lock()
            .expect("node state mutex poisoned")
            .node_id;
        let mut current_object = canopy_rpc::Shared::null();
        for index in 0..instruction_count.max(0) {
            let instruction = Instruction {
                instruction_id: index,
                operation: if index % 2 == 0 {
                    "CREATE_OBJECT".to_string()
                } else {
                    "PASS_TO_TARGET".to_string()
                },
                target_value: index,
            };
            let mut output_object = canopy_rpc::Shared::null();
            if instruction.operation.starts_with("PASS_TO") {
                let _ = target_node.receive_object(current_object.clone(), sender_node_id);
            } else if self.execute_instruction(
                instruction,
                current_object.clone(),
                &mut output_object,
            ) == error_codes::OK()
                && output_object.as_ref().is_some()
            {
                current_object = output_object;
            }
        }
        error_codes::OK()
    }

    fn execute_instruction(
        &self,
        instruction: Instruction,
        input_object: canopy_rpc::SharedPtr<dyn ISharedObject>,
        output_object: &mut canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        *output_object = input_object.clone();
        match instruction.operation.as_str() {
            "CREATE_CAPABILITY" => match instruction.target_value.rem_euclid(3) {
                0 => return self.create_local_factory(),
                1 => return self.create_local_cache(),
                _ => return self.create_local_worker(),
            },
            "CREATE_OBJECT" => {
                let factory = self
                    .state
                    .lock()
                    .expect("node state mutex poisoned")
                    .local_factory
                    .clone();
                let Some(factory) = factory.as_ref() else {
                    return error_codes::OK();
                };
                let mut ignored_factory_object = canopy_rpc::Shared::null();
                let error_code = factory.create_shared_object(
                    instruction.target_value,
                    format!(
                        "obj_{}_{}",
                        self.state
                            .lock()
                            .expect("node state mutex poisoned")
                            .node_id,
                        instruction.target_value
                    ),
                    instruction.target_value * 10,
                    &mut ignored_factory_object,
                );
                if error_code != error_codes::OK() {
                    return error_code;
                }
                let object = RustSharedObject::default();
                let _ = object.set_value(instruction.target_value * 10);
                let shared = if let Some(service_runtime) = &self.service_runtime {
                    let rpc_object = fg::i_shared_object::make_rpc_object(object.clone());
                    let object_id = service_runtime.generate_new_object_id();
                    if service_runtime
                        .register_rpc_object(object_id, rpc_object)
                        .is_ok()
                    {
                        canopy_rpc::get_interface_view::<dyn ISharedObject>(
                            service_runtime.as_ref(),
                            object_id,
                            canopy_rpc::InterfaceOrdinal::new(fg::i_shared_object::ID_RPC_V3),
                        )
                        .map(canopy_rpc::Shared::from_arc)
                        .unwrap_or_else(|_| {
                            let object: Arc<dyn ISharedObject> = Arc::new(object);
                            canopy_rpc::Shared::from_arc(object)
                        })
                    } else {
                        let object: Arc<dyn ISharedObject> = Arc::new(object);
                        canopy_rpc::Shared::from_arc(object)
                    }
                } else {
                    let object: Arc<dyn ISharedObject> = Arc::new(object);
                    canopy_rpc::Shared::from_arc(object)
                };
                self.state
                    .lock()
                    .expect("node state mutex poisoned")
                    .created_objects
                    .push(shared.clone());
                *output_object = shared;
            }
            "PROCESS_OBJECT" => {
                let worker = self
                    .state
                    .lock()
                    .expect("node state mutex poisoned")
                    .local_worker
                    .clone();
                if let Some(worker) = worker.as_ref() {
                    let object = if input_object.as_ref().is_some() {
                        input_object
                    } else {
                        let state = self.state.lock().expect("node state mutex poisoned");
                        state
                            .created_objects
                            .get(
                                (instruction.target_value as usize)
                                    % state.created_objects.len().max(1),
                            )
                            .cloned()
                            .unwrap_or_else(canopy_rpc::Shared::null)
                    };
                    if object.as_ref().is_some() {
                        let _ = worker.process_object(object, instruction.target_value);
                    }
                }
            }
            "STORE_OBJECT" => {
                let cache = self
                    .state
                    .lock()
                    .expect("node state mutex poisoned")
                    .local_cache
                    .clone();
                if let Some(cache) = cache.as_ref() {
                    let object = if input_object.as_ref().is_some() {
                        input_object
                    } else {
                        let state = self.state.lock().expect("node state mutex poisoned");
                        state
                            .created_objects
                            .get(
                                (instruction.target_value as usize)
                                    % state.created_objects.len().max(1),
                            )
                            .cloned()
                            .unwrap_or_else(canopy_rpc::Shared::null)
                    };
                    if object.as_ref().is_some() {
                        let _ = cache.store_object(instruction.target_value, object);
                    }
                }
            }
            "FORK_CHILD" => {
                let mut child = canopy_rpc::Shared::null();
                let _ = self.create_child_node(
                    NodeType::WorkerNode,
                    instruction.target_value as u64,
                    true,
                    &mut child,
                );
            }
            "PASS_TO_RANDOM_CHILD" => {
                let child = {
                    let state = self.state.lock().expect("node state mutex poisoned");
                    if state.child_nodes.is_empty() {
                        canopy_rpc::Shared::null()
                    } else {
                        state.child_nodes[0].clone()
                    }
                };
                if let Some(child) = child.as_ref() {
                    let sender_node_id = self
                        .state
                        .lock()
                        .expect("node state mutex poisoned")
                        .node_id;
                    let _ = child.receive_object(input_object, sender_node_id);
                }
            }
            "PASS_TO_PARENT" => {
                let parent = self
                    .state
                    .lock()
                    .expect("node state mutex poisoned")
                    .parent_node
                    .clone();
                if let Some(parent) = parent.as_ref() {
                    let sender_node_id = self
                        .state
                        .lock()
                        .expect("node state mutex poisoned")
                        .node_id;
                    let _ = parent.receive_object(input_object, sender_node_id);
                }
            }
            "PASS_TO_RANDOM_SIBLING" => {
                let parent = self
                    .state
                    .lock()
                    .expect("node state mutex poisoned")
                    .parent_node
                    .clone();
                if let Some(parent) = parent.as_ref() {
                    let mut sibling_count = 0;
                    if parent.get_cached_children_count(&mut sibling_count) == error_codes::OK()
                        && sibling_count > 1
                    {
                        for index in 0..sibling_count {
                            let mut sibling = canopy_rpc::Shared::null();
                            if parent.get_cached_child_by_index(index, &mut sibling)
                                != error_codes::OK()
                            {
                                continue;
                            }
                            let Some(sibling_ref) = sibling.as_ref() else {
                                continue;
                            };
                            let mut sibling_id = 0;
                            let mut sibling_type = NodeType::RootNode;
                            let mut sibling_connections = 0;
                            let mut sibling_objects = 0;
                            if sibling_ref.get_node_status(
                                &mut sibling_type,
                                &mut sibling_id,
                                &mut sibling_connections,
                                &mut sibling_objects,
                            ) == error_codes::OK()
                                && sibling_id
                                    != self
                                        .state
                                        .lock()
                                        .expect("node state mutex poisoned")
                                        .node_id
                            {
                                let sender_node_id = self
                                    .state
                                    .lock()
                                    .expect("node state mutex poisoned")
                                    .node_id;
                                let _ = sibling_ref.receive_object(input_object, sender_node_id);
                                break;
                            }
                        }
                    }
                }
            }
            _ => {}
        }
        error_codes::OK()
    }

    fn connect_to_node(&self, target_node: canopy_rpc::SharedPtr<dyn IAutonomousNode>) -> i32 {
        if target_node.as_ref().is_none() {
            return error_codes::INVALID_DATA();
        }
        let mut state = self.state.lock().expect("node state mutex poisoned");
        state.connections.push(target_node);
        error_codes::OK()
    }

    fn pass_object_to_connected(
        &self,
        connection_index: i32,
        object: canopy_rpc::SharedPtr<dyn ISharedObject>,
    ) -> i32 {
        let target = {
            let state = self.state.lock().expect("node state mutex poisoned");
            if connection_index < 0 || connection_index as usize >= state.connections.len() {
                return error_codes::INVALID_DATA();
            }
            state.connections[connection_index as usize].clone()
        };
        let Some(target) = target.as_ref() else {
            return error_codes::OBJECT_GONE();
        };
        let sender_node_id = self
            .state
            .lock()
            .expect("node state mutex poisoned")
            .node_id;
        target.receive_object(object, sender_node_id)
    }

    fn receive_object(
        &self,
        object: canopy_rpc::SharedPtr<dyn ISharedObject>,
        _sender_node_id: u64,
    ) -> i32 {
        let mut state = self.state.lock().expect("node state mutex poisoned");
        if object.as_ref().is_some() {
            state.created_objects.push(object);
        }
        state.signals_received += 1;
        error_codes::OK()
    }

    fn get_node_status(
        &self,
        current_type: &mut NodeType,
        current_id: &mut u64,
        connections_count: &mut i32,
        objects_held: &mut i32,
    ) -> i32 {
        let state = self.state.lock().expect("node state mutex poisoned");
        *current_type = state.node_type;
        *current_id = state.node_id;
        *connections_count = state.connections.len() as i32
            + i32::from(state.local_factory.as_ref().is_some())
            + i32::from(state.local_cache.as_ref().is_some())
            + i32::from(state.local_worker.as_ref().is_some());
        *objects_held = state.signals_received;
        error_codes::OK()
    }

    fn create_child_node(
        &self,
        child_type: NodeType,
        child_zone_id: u64,
        cache_locally: bool,
        child_node: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> i32 {
        let _ = cache_locally;
        *child_node = canopy_rpc::Shared::null();
        let child = match self.autonomous_node_ptr(child_type, child_zone_id, self.local_node_ptr())
        {
            Ok(child) => child,
            Err(error_code) => return error_code,
        };
        *child_node = child;
        error_codes::OK()
    }

    fn request_child_creation(
        &self,
        target_parent: canopy_rpc::SharedPtr<dyn IAutonomousNode>,
        child_type: NodeType,
        child_zone_id: u64,
        child_proxy: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> i32 {
        let Some(target_parent) = target_parent.as_ref() else {
            *child_proxy = canopy_rpc::Shared::null();
            return error_codes::INVALID_DATA();
        };
        target_parent.create_child_node(child_type, child_zone_id, false, child_proxy)
    }

    fn get_cached_children_count(&self, count: &mut i32) -> i32 {
        *count = self
            .state
            .lock()
            .expect("node state mutex poisoned")
            .child_nodes
            .len() as i32;
        error_codes::OK()
    }

    fn get_cached_child_by_index(
        &self,
        index: i32,
        child: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>,
    ) -> i32 {
        let state = self.state.lock().expect("node state mutex poisoned");
        if index < 0 || index as usize >= state.child_nodes.len() {
            *child = canopy_rpc::Shared::null();
            return error_codes::INVALID_DATA();
        }
        *child = state.child_nodes[index as usize].clone();
        error_codes::OK()
    }

    fn get_parent_node(&self, parent: &mut canopy_rpc::SharedPtr<dyn IAutonomousNode>) -> i32 {
        *parent = self
            .state
            .lock()
            .expect("node state mutex poisoned")
            .parent_node
            .clone();
        error_codes::OK()
    }

    fn set_parent_node(&self, parent: canopy_rpc::SharedPtr<dyn IAutonomousNode>) -> i32 {
        self.state
            .lock()
            .expect("node state mutex poisoned")
            .parent_node = parent;
        error_codes::OK()
    }
}

fn mut_ptr<'a, T>(ptr: *mut T) -> Option<&'a mut T> {
    unsafe { ptr.as_mut() }
}

fn const_ptr<'a, T>(ptr: *const T) -> Option<&'a T> {
    unsafe { ptr.as_ref() }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_init(params: *mut dll::CanopyDllInitParams) -> i32 {
    let Some(params) = mut_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let init_params = *params;
    let child_zone = dll::decode_zone_or_else(init_params.child_zone, || {
        Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Local,
                0,
                vec![],
                DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
                77,
                DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
                0,
                vec![],
            ))
            .expect("fallback child zone"),
        )
    });
    let parent_zone = dll::decode_zone_or_else(init_params.parent_zone, Zone::default);
    let service = ChildService::new_shared("rust-fuzz-child", child_zone.clone(), parent_zone);
    let output_object = child_output_object(&init_params);
    dll::dll_init::<dll::ChildServiceEndpoint, _>(params, |_parent_transport, _input_descr| {
        Ok((
            {
                let node = RustFuzzNode::with_service_and_object(
                    service.clone(),
                    output_object.get_object_id(),
                );
                let (endpoint, _root_object) = dll::attach_child_zone_with_exported_object(
                    "rust-fuzz-child-parent",
                    service,
                    &init_params,
                    output_object.get_object_id(),
                    make_fuzz_node_rpc_object(node),
                )?;
                endpoint
            },
            output_object,
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_destroy(child_ctx: dll::CanopyChildContext) {
    dll::dll_destroy::<dll::ChildServiceEndpoint>(child_ctx);
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_send(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopySendParams,
    result: *mut dll::CanopySendResult,
) -> i32 {
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };
    dll::dll_send::<dll::ChildServiceEndpoint>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_post(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyPostParams,
) {
    if let Some(params) = const_ptr(params) {
        dll::dll_post::<dll::ChildServiceEndpoint>(child_ctx, params);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_try_cast(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyTryCastParams,
    result: *mut dll::CanopyStandardResult,
) -> i32 {
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };
    dll::dll_try_cast::<dll::ChildServiceEndpoint>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_add_ref(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyAddRefParams,
    result: *mut dll::CanopyStandardResult,
) -> i32 {
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };
    dll::dll_add_ref::<dll::ChildServiceEndpoint>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_release(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyReleaseParams,
    result: *mut dll::CanopyStandardResult,
) -> i32 {
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };
    dll::dll_release::<dll::ChildServiceEndpoint>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_object_released(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyObjectReleasedParams,
) {
    if let Some(params) = const_ptr(params) {
        dll::dll_object_released::<dll::ChildServiceEndpoint>(child_ctx, params);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_transport_down(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyTransportDownParams,
) {
    if let Some(params) = const_ptr(params) {
        dll::dll_transport_down::<dll::ChildServiceEndpoint>(child_ctx, params);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_get_new_zone_id(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyGetNewZoneIdParams,
    result: *mut dll::CanopyNewZoneIdResult,
) -> i32 {
    let Some(params) = const_ptr(params) else {
        return error_codes::INVALID_DATA();
    };
    let Some(result) = mut_ptr(result) else {
        return error_codes::INVALID_DATA();
    };
    dll::dll_get_new_zone_id::<dll::ChildServiceEndpoint>(child_ctx, params, result)
}
