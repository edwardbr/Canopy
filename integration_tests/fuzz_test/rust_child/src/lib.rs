use canopy_protobuf_runtime_probe::fuzz_test::fuzz_test::{
    __Generated as fg, IAutonomousNode, ICleanup, IGarbageCollector, ISharedObject, Instruction,
    NodeType,
};
use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddRefParams, AddressType, DefaultValues, GetNewZoneIdParams, IMarshaller, Object,
    ObjectReleasedParams, PostParams, ReleaseParams, RemoteObject, SendParams, SendResult, Service,
    StandardResult, Transport, TransportDownParams, TransportStatus, TryCastParams, Zone,
    ZoneAddress, ZoneAddressArgs,
};
use canopy_transport_dynamic_library as dll;
use std::sync::{Arc, Mutex};

fn zone_from_ffi(value: dll::ffi::CanopyZone) -> Zone {
    let raw_blob = value.address.blob;
    if raw_blob.data.is_null() || raw_blob.size == 0 {
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
    } else {
        let blob = unsafe { std::slice::from_raw_parts(raw_blob.data, raw_blob.size).to_vec() };
        Zone::new(ZoneAddress::new(blob))
    }
}

fn child_output_object(params: &dll::CanopyDllInitParams) -> RemoteObject {
    let child_zone = zone_from_ffi(params.child_zone);
    child_zone
        .with_object(Object::new(100))
        .expect("child output object")
}

#[derive(Clone)]
struct RustFuzzNode {
    state: Arc<Mutex<RustFuzzNodeState>>,
    service: Option<Arc<Service>>,
}

struct RustFuzzNodeState {
    node_type: NodeType,
    node_id: u64,
    signals_received: i32,
    connections: Vec<canopy_rpc::SharedPtr<dyn IAutonomousNode>>,
    child_nodes: Vec<canopy_rpc::SharedPtr<dyn IAutonomousNode>>,
    created_objects: Vec<canopy_rpc::SharedPtr<dyn ISharedObject>>,
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
                parent_node: canopy_rpc::Shared::null(),
                cleanup_called: false,
            })),
            service: None,
        }
    }
}

impl RustFuzzNode {
    fn with_service(service: Arc<Service>) -> Self {
        Self {
            service: Some(service),
            ..Self::default()
        }
    }

    fn shared_object_ptr(&self, value: i32) -> canopy_rpc::SharedPtr<dyn ISharedObject> {
        let object = RustSharedObject::default();
        let _ = object.set_value(value);
        if let Some(service) = &self.service {
            let rpc_object = fg::ISharedObject::make_rpc_object(object.clone());
            let object_id = service.generate_new_object_id();
            let stub = Arc::new(Mutex::new(canopy_rpc::internal::ObjectStub::with_target(
                object_id, rpc_object,
            )));
            if service.register_stub(&stub) == error_codes::OK() {
                if let Ok(view) = service.lookup_local_interface_view::<dyn ISharedObject>(
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
    ) -> canopy_rpc::SharedPtr<dyn IAutonomousNode> {
        let child = RustFuzzNode {
            state: Arc::new(Mutex::new(RustFuzzNodeState {
                node_type,
                node_id,
                signals_received: 0,
                connections: Vec::new(),
                child_nodes: Vec::new(),
                created_objects: Vec::new(),
                parent_node: canopy_rpc::Shared::null(),
                cleanup_called: false,
            })),
            service: self.service.clone(),
        };

        if let Some(service) = &self.service {
            let rpc_object = fg::IAutonomousNode::make_rpc_object(child.clone());
            let object_id = service.generate_new_object_id();
            let stub = Arc::new(Mutex::new(canopy_rpc::internal::ObjectStub::with_target(
                object_id, rpc_object,
            )));
            if service.register_stub(&stub) == error_codes::OK() {
                if let Ok(view) = service.lookup_local_interface_view::<dyn IAutonomousNode>(
                    object_id,
                    canopy_rpc::InterfaceOrdinal::new(fg::IAutonomousNode::ID_RPC_V3),
                ) {
                    return canopy_rpc::Shared::from_arc(view);
                }
            }
        }

        let child: Arc<dyn IAutonomousNode> = Arc::new(child);
        canopy_rpc::Shared::from_arc(child)
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
        let child = self.autonomous_node_ptr(child_type, child_zone_id);
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

#[derive(Clone)]
struct RustFuzzMarshaller {
    node: RustFuzzNode,
    service: Arc<Service>,
    output_object: RemoteObject,
}

#[derive(Clone, Copy)]
struct ParentCallbackMarshaller(dll::ffi::ParentCallbacks);

// The parent callback table is a stable C ABI handle owned by the C++ parent
// side for the lifetime of the child context. The Rust Transport requires a
// Send + Sync marshaller, matching the C++ transport ownership model.
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

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> canopy_rpc::NewZoneIdResult {
        self.0.get_new_zone_id(params)
    }
}

impl RustFuzzMarshaller {
    fn proto_remote_object(
        value: &RemoteObject,
    ) -> canopy_protobuf_runtime_probe::__canopy_protobuf::rpc::remote_object {
        let mut remote_object =
            canopy_protobuf_runtime_probe::__canopy_protobuf::rpc::remote_object::new();
        let mut zone_address =
            canopy_protobuf_runtime_probe::__canopy_protobuf::rpc::zone_address::new();
        zone_address.set_blob(value.get_address().get_blob().to_vec());
        remote_object.set_addr_(zone_address);
        remote_object
    }

    fn encode_create_child_node_ok(&self, caller_zone_id: canopy_rpc::CallerZone) -> SendResult {
        let mut response =
            canopy_protobuf_runtime_probe::__canopy_protobuf::fuzz_test::i_autonomous_node_create_child_nodeResponse::new();
        let parent_view = self
            .service
            .lookup_local_interface_view::<dyn IAutonomousNode>(
                self.output_object.get_object_id(),
                canopy_rpc::InterfaceOrdinal::new(fg::IAutonomousNode::ID_RPC_V3),
            )
            .map(canopy_rpc::Shared::from_arc)
            .unwrap_or_else(|_| {
                let node: Arc<dyn IAutonomousNode> = Arc::new(self.node.clone());
                canopy_rpc::Shared::from_arc(node)
            });

        let child_id = self.service.generate_new_object_id();
        let child = RustFuzzNode {
            state: Arc::new(Mutex::new(RustFuzzNodeState {
                node_type: NodeType::WorkerNode,
                node_id: child_id.get_val(),
                signals_received: 0,
                connections: Vec::new(),
                child_nodes: Vec::new(),
                created_objects: Vec::new(),
                parent_node: parent_view,
                cleanup_called: false,
            })),
            service: Some(self.service.clone()),
        };
        let child_rpc_object = fg::IAutonomousNode::make_rpc_object(child);
        let child_stub = Arc::new(Mutex::new(canopy_rpc::internal::ObjectStub::with_target(
            child_id,
            child_rpc_object,
        )));
        if self.service.register_stub(&child_stub) != error_codes::OK() {
            response.set_result(error_codes::REFERENCE_COUNT_ERROR());
            return match canopy_rpc::serialization::protobuf::serialize_generated_message(&response)
            {
                Ok(out_buf) => SendResult::new(error_codes::OK(), out_buf, Vec::new()),
                Err(error_code) => SendResult::new(error_code, Vec::new(), Vec::new()),
            };
        }
        child_stub
            .lock()
            .expect("child stub mutex poisoned")
            .add_ref(canopy_rpc::InterfacePointerKind::Shared, caller_zone_id);

        let child_descriptor = self
            .service
            .zone_id()
            .with_object(child_id)
            .expect("registered child descriptor");
        let child_view = self
            .service
            .lookup_local_interface_view::<dyn IAutonomousNode>(
                child_id,
                canopy_rpc::InterfaceOrdinal::new(fg::IAutonomousNode::ID_RPC_V3),
            )
            .map(canopy_rpc::Shared::from_arc)
            .unwrap_or_else(|_| canopy_rpc::Shared::null());
        self.node
            .state
            .lock()
            .expect("node state mutex poisoned")
            .parent_node = child_view.clone();
        self.node
            .state
            .lock()
            .expect("node state mutex poisoned")
            .child_nodes
            .push(child_view);
        response.set_child_node(Self::proto_remote_object(&child_descriptor));
        response.set_result(error_codes::OK());
        match canopy_rpc::serialization::protobuf::serialize_generated_message(&response) {
            Ok(out_buf) => SendResult::new(error_codes::OK(), out_buf, Vec::new()),
            Err(error_code) => SendResult::new(error_code, Vec::new(), Vec::new()),
        }
    }

    fn dispatch_generated(&self, params: SendParams) -> Option<SendResult> {
        if params.encoding_type != canopy_rpc::Encoding::ProtocolBuffers {
            return None;
        }

        let context = canopy_rpc::DispatchContext::from(&params)
            .with_owner_service_ptr(Some(Arc::as_ptr(&self.service) as usize));
        if fg::IAutonomousNode::matches_interface_id(params.interface_id) {
            if params.method_id.get_val() == 8 {
                return Some(self.encode_create_child_node_ok(params.caller_zone_id.clone()));
            }
            if params.remote_object_id.get_object_id().is_set()
                && self
                    .service
                    .get_object(params.remote_object_id.get_object_id())
                    .is_some()
            {
                return Some(self.service.send(params));
            }
            return Some(IAutonomousNode::__rpc_dispatch_generated(
                &self.node, &context, params,
            ));
        }

        if fg::ICleanup::matches_interface_id(params.interface_id) {
            return Some(ICleanup::__rpc_dispatch_generated(
                &self.node, &context, params,
            ));
        }

        None
    }
}

impl IMarshaller for RustFuzzMarshaller {
    fn send(&self, params: SendParams) -> SendResult {
        self.dispatch_generated(params).unwrap_or_else(|| {
            SendResult::new(error_codes::INVALID_INTERFACE_ID(), Vec::new(), Vec::new())
        })
    }

    fn post(&self, _params: PostParams) {}

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        let error_code = if fg::IAutonomousNode::matches_interface_id(params.interface_id)
            || fg::ICleanup::matches_interface_id(params.interface_id)
        {
            error_codes::OK()
        } else {
            error_codes::INVALID_INTERFACE_ID()
        };
        StandardResult::new(error_code, Vec::new())
    }

    fn add_ref(&self, _params: AddRefParams) -> StandardResult {
        StandardResult::new(error_codes::OK(), Vec::new())
    }

    fn release(&self, _params: ReleaseParams) -> StandardResult {
        StandardResult::new(error_codes::OK(), Vec::new())
    }

    fn object_released(&self, _params: ObjectReleasedParams) {}

    fn transport_down(&self, _params: TransportDownParams) {}

    fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> canopy_rpc::NewZoneIdResult {
        canopy_rpc::NewZoneIdResult::new(error_codes::ZONE_NOT_FOUND(), Zone::default(), Vec::new())
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
    let child_zone = zone_from_ffi(params.child_zone);
    let parent_zone = zone_from_ffi(params.parent_zone);
    let service = Service::new_shared("rust-fuzz-child", child_zone.clone());
    let parent_callbacks =
        ParentCallbackMarshaller(dll::ffi::ParentCallbacks::from_init_params(params));
    let parent_transport = Transport::new(
        "rust-fuzz-child-parent",
        child_zone,
        parent_zone.clone(),
        Arc::downgrade(&service),
        Arc::new(parent_callbacks),
    );
    parent_transport.set_status(TransportStatus::Connected);
    service.add_transport(parent_zone, parent_transport);
    let output_object = child_output_object(params);
    dll::dll_init::<RustFuzzMarshaller, _>(params, |_parent_transport, _input_descr| {
        Ok((
            {
                let node = RustFuzzNode::with_service(service.clone());
                let root_object = fg::IAutonomousNode::make_rpc_object(node.clone());
                let root_stub =
                    Arc::new(Mutex::new(canopy_rpc::internal::ObjectStub::with_target(
                        output_object.get_object_id(),
                        root_object,
                    )));
                let _ = service.register_stub(&root_stub);
                RustFuzzMarshaller {
                    node,
                    service,
                    output_object: output_object.clone(),
                }
            },
            output_object,
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_destroy(child_ctx: dll::CanopyChildContext) {
    dll::dll_destroy::<RustFuzzMarshaller>(child_ctx);
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
    dll::dll_send::<RustFuzzMarshaller>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_post(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyPostParams,
) {
    if let Some(params) = const_ptr(params) {
        dll::dll_post::<RustFuzzMarshaller>(child_ctx, params);
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
    dll::dll_try_cast::<RustFuzzMarshaller>(child_ctx, params, result)
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
    dll::dll_add_ref::<RustFuzzMarshaller>(child_ctx, params, result)
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
    dll::dll_release::<RustFuzzMarshaller>(child_ctx, params, result)
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_object_released(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyObjectReleasedParams,
) {
    if let Some(params) = const_ptr(params) {
        dll::dll_object_released::<RustFuzzMarshaller>(child_ctx, params);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopy_dll_transport_down(
    child_ctx: dll::CanopyChildContext,
    params: *const dll::CanopyTransportDownParams,
) {
    if let Some(params) = const_ptr(params) {
        dll::dll_transport_down::<RustFuzzMarshaller>(child_ctx, params);
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
    dll::dll_get_new_zone_id::<RustFuzzMarshaller>(child_ctx, params, result)
}
