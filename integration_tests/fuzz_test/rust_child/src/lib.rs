use canopy_protobuf_runtime_probe::fuzz_test::fuzz_test::{
    __generated as fg, IAutonomousNode, ICleanup, IGarbageCollector, ISharedObject, Instruction,
    NodeType,
};
use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddressType, ChildService, DefaultValues, Encoding, GeneratedRpcCallContext,
    InterfacePointerKind, Object, RemoteObject, SendParams, SendResult, ServiceProxy,
    ServiceRuntime, Zone, ZoneAddress, ZoneAddressArgs,
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
            canopy_rpc::InterfaceOrdinal::new(fg::IAutonomousNode::ID_RPC_V3),
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
            let rpc_object = fg::ISharedObject::make_rpc_object(object.clone());
            let object_id = service_runtime.generate_new_object_id();
            let stub = Arc::new(Mutex::new(canopy_rpc::internal::ObjectStub::with_target(
                object_id, rpc_object,
            )));
            if service_runtime.register_stub(&stub) == error_codes::OK() {
                if let Ok(view) = canopy_rpc::get_interface_view::<dyn ISharedObject>(
                    service_runtime.as_ref(),
                    object_id,
                    canopy_rpc::InterfaceOrdinal::new(fg::ISharedObject::ID_RPC_V3),
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

        let service_proxy = ServiceProxy::with_transport(
            parent_runtime.clone(),
            bound_zone.zone().child_transport().transport(),
            GeneratedRpcCallContext {
                protocol_version: DefaultValues::VERSION_3 as u64,
                encoding_type: Encoding::ProtocolBuffers,
                tag: 0,
                caller_zone_id: parent_runtime.zone_id(),
                remote_object_id: remote_object.clone(),
            },
        );
        let caller = service_proxy
            .proxy_for_remote_object_with_ref(remote_object, InterfacePointerKind::Shared);
        let proxy: Arc<dyn IAutonomousNode> =
            Arc::new(fg::IAutonomousNode::ProxySkeleton::with_caller(caller));
        self.state
            .lock()
            .expect("node state mutex poisoned")
            .child_zones
            .push(bound_zone);
        Ok(canopy_rpc::Shared::from_arc(proxy))
    }
}

impl canopy_rpc::CreateLocalProxy for RustFuzzNode {}

impl canopy_rpc::CastingInterface for RustFuzzNode {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::IAutonomousNode::matches_interface_id(interface_id)
            || fg::ICleanup::matches_interface_id(interface_id)
    }
}

impl canopy_rpc::GeneratedRustInterface for RustFuzzNode {
    fn interface_name() -> &'static str {
        fg::IAutonomousNode::NAME
    }

    fn get_id(rpc_version: u64) -> u64 {
        if rpc_version >= 3 {
            fg::IAutonomousNode::ID_RPC_V3
        } else {
            fg::IAutonomousNode::ID_RPC_V2
        }
    }

    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        fg::IAutonomousNode::interface_binding::METHODS
    }
}

struct RustFuzzNodeAdapter;

impl canopy_rpc::LocalObjectAdapter<RustFuzzNode> for RustFuzzNodeAdapter {
    fn interface_name() -> &'static str {
        fg::IAutonomousNode::NAME
    }

    fn get_id(rpc_version: u64) -> u64 {
        if rpc_version >= 3 {
            fg::IAutonomousNode::ID_RPC_V3
        } else {
            fg::IAutonomousNode::ID_RPC_V2
        }
    }

    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        fg::IAutonomousNode::interface_binding::METHODS
    }

    fn supports_interface(interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::IAutonomousNode::matches_interface_id(interface_id)
            || fg::ICleanup::matches_interface_id(interface_id)
    }

    fn dispatch(
        implementation: &RustFuzzNode,
        context: &canopy_rpc::DispatchContext,
        params: SendParams,
    ) -> SendResult {
        if fg::IAutonomousNode::matches_interface_id(params.interface_id) {
            return IAutonomousNode::__rpc_dispatch_generated(implementation, context, params);
        }
        if fg::ICleanup::matches_interface_id(params.interface_id) {
            return ICleanup::__rpc_dispatch_generated(implementation, context, params);
        }
        SendResult::new(error_codes::INVALID_INTERFACE_ID(), Vec::new(), Vec::new())
    }

    fn method_metadata(
        interface_id: canopy_rpc::InterfaceOrdinal,
    ) -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        if fg::IAutonomousNode::matches_interface_id(interface_id) {
            return fg::IAutonomousNode::interface_binding::METHODS;
        }
        if fg::ICleanup::matches_interface_id(interface_id) {
            return fg::ICleanup::interface_binding::METHODS;
        }
        &[]
    }

    fn local_interface_view(
        object: Arc<canopy_rpc::RpcBase<RustFuzzNode, Self>>,
        interface_id: canopy_rpc::InterfaceOrdinal,
    ) -> Option<Arc<dyn std::any::Any + Send + Sync>> {
        if fg::IAutonomousNode::matches_interface_id(interface_id) {
            let view: Arc<dyn IAutonomousNode> = Arc::new(RustFuzzAutonomousNodeView { object });
            return Some(Arc::new(canopy_rpc::internal::LocalInterfaceView::new(
                view,
            )));
        }
        if fg::ICleanup::matches_interface_id(interface_id) {
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
        fg::IAutonomousNode::matches_interface_id(interface_id)
    }

    fn __rpc_call(&self, params: SendParams) -> SendResult {
        canopy_rpc::CastingInterface::__rpc_call(self.object.as_ref(), params)
    }

    fn __rpc_local_object_stub(&self) -> Option<Arc<Mutex<canopy_rpc::internal::ObjectStub>>> {
        canopy_rpc::internal::get_object_stub(self.object.as_ref())
    }
}

impl canopy_rpc::GeneratedRustInterface for RustFuzzAutonomousNodeView {
    fn interface_name() -> &'static str {
        fg::IAutonomousNode::NAME
    }

    fn get_id(rpc_version: u64) -> u64 {
        if rpc_version >= 3 {
            fg::IAutonomousNode::ID_RPC_V3
        } else {
            fg::IAutonomousNode::ID_RPC_V2
        }
    }

    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        fg::IAutonomousNode::interface_binding::METHODS
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
        fg::ICleanup::matches_interface_id(interface_id)
    }

    fn __rpc_call(&self, params: SendParams) -> SendResult {
        canopy_rpc::CastingInterface::__rpc_call(self.object.as_ref(), params)
    }

    fn __rpc_local_object_stub(&self) -> Option<Arc<Mutex<canopy_rpc::internal::ObjectStub>>> {
        canopy_rpc::internal::get_object_stub(self.object.as_ref())
    }
}

impl canopy_rpc::GeneratedRustInterface for RustFuzzCleanupView {
    fn interface_name() -> &'static str {
        fg::ICleanup::NAME
    }

    fn get_id(rpc_version: u64) -> u64 {
        if rpc_version >= 3 {
            fg::ICleanup::ID_RPC_V3
        } else {
            fg::ICleanup::ID_RPC_V2
        }
    }

    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        fg::ICleanup::interface_binding::METHODS
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

impl canopy_rpc::CreateLocalProxy for RustSharedObject {}

impl canopy_rpc::CastingInterface for RustSharedObject {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        fg::ISharedObject::matches_interface_id(interface_id)
    }
}

impl canopy_rpc::GeneratedRustInterface for RustSharedObject {
    fn interface_name() -> &'static str {
        fg::ISharedObject::NAME
    }

    fn get_id(rpc_version: u64) -> u64 {
        if rpc_version >= 3 {
            fg::ISharedObject::ID_RPC_V3
        } else {
            fg::ISharedObject::ID_RPC_V2
        }
    }

    fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] {
        fg::ISharedObject::interface_binding::METHODS
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
            if instruction.operation == "PASS_TO_TARGET" {
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
            "CREATE_OBJECT" => {
                let shared = self.shared_object_ptr(instruction.target_value * 10);
                self.state
                    .lock()
                    .expect("node state mutex poisoned")
                    .created_objects
                    .push(shared.clone());
                *output_object = shared;
            }
            "PROCESS_OBJECT" => {
                if let Some(object) = input_object.as_ref() {
                    let mut value = 0;
                    if object.get_value(&mut value) == error_codes::OK() {
                        let _ = object.set_value(value + instruction.target_value);
                    }
                }
            }
            "STORE_OBJECT" => {
                if input_object.as_ref().is_some() {
                    self.state
                        .lock()
                        .expect("node state mutex poisoned")
                        .created_objects
                        .push(input_object.clone());
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
        *connections_count = state.connections.len() as i32;
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
        *child_node = canopy_rpc::Shared::null();
        let child = match self.autonomous_node_ptr(child_type, child_zone_id, self.local_node_ptr())
        {
            Ok(child) => child,
            Err(error_code) => return error_code,
        };
        if cache_locally {
            self.state
                .lock()
                .expect("node state mutex poisoned")
                .child_nodes
                .push(child.clone());
        }
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
