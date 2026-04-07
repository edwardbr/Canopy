use canopy_protobuf_runtime_probe::fuzz_test::fuzz_test::{
    __Generated as fg, IAutonomousNode, ICleanup, ISharedObject, Instruction, NodeType,
};
use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddRefParams, AddressType, DefaultValues, GetNewZoneIdParams, IMarshaller, Object,
    ObjectReleasedParams, PostParams, ReleaseParams, RemoteObject, SendParams, SendResult, Service,
    StandardResult, Transport, TransportDownParams, TransportStatus, TryCastParams, Zone,
    ZoneAddress, ZoneAddressArgs,
};
use canopy_transport_dynamic_library as dll;
use std::sync::Arc;

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

#[derive(Clone, Default)]
struct RustFuzzNode;

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
struct RustSharedObject;

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
        input_value
    }

    fn get_stats(&self, count: &mut i32) -> i32 {
        *count = 0;
        error_codes::OK()
    }

    fn set_value(&self, _new_value: i32) -> i32 {
        error_codes::OK()
    }

    fn get_value(&self, value: &mut i32) -> i32 {
        *value = 0;
        error_codes::OK()
    }
}

impl ICleanup for RustFuzzNode {
    type CleanupCollectorIface = fg::IGarbageCollector::ProxySkeleton;

    fn cleanup<CollectorIface>(&self, _collector: canopy_rpc::SharedPtr<CollectorIface>) -> i32
    where
        CollectorIface: canopy_protobuf_runtime_probe::fuzz_test::fuzz_test::IGarbageCollector,
    {
        error_codes::OK()
    }
}

impl IAutonomousNode for RustFuzzNode {
    type RunScriptTargetNodeIface = RustFuzzNode;
    type ExecuteInstructionInputObjectIface = RustSharedObject;
    type ExecuteInstructionOutputObjectIface = RustSharedObject;
    type ConnectToNodeTargetNodeIface = RustFuzzNode;
    type PassObjectToConnectedObjectIface = RustSharedObject;
    type ReceiveObjectObjectIface = RustSharedObject;
    type CreateChildNodeChildNodeIface = RustFuzzNode;
    type RequestChildCreationTargetParentIface = RustFuzzNode;
    type RequestChildCreationChildProxyIface = RustFuzzNode;
    type GetCachedChildByIndexChildIface = RustFuzzNode;
    type GetParentNodeParentIface = RustFuzzNode;
    type SetParentNodeParentIface = RustFuzzNode;

    fn initialize_node(&self, _type: NodeType, _node_id: u64) -> i32 {
        error_codes::OK()
    }

    fn run_script<TargetNodeIface>(
        &self,
        _target_node: canopy_rpc::SharedPtr<TargetNodeIface>,
        _instruction_count: i32,
    ) -> i32
    where
        TargetNodeIface: IAutonomousNode,
    {
        error_codes::OK()
    }

    fn execute_instruction<InputObjectIface>(
        &self,
        _instruction: Instruction,
        _input_object: canopy_rpc::SharedPtr<InputObjectIface>,
        output_object: &mut canopy_rpc::SharedPtr<RustSharedObject>,
    ) -> i32
    where
        InputObjectIface: ISharedObject,
    {
        *output_object = canopy_rpc::Shared::null();
        error_codes::OK()
    }

    fn connect_to_node<TargetNodeIface>(
        &self,
        _target_node: canopy_rpc::SharedPtr<TargetNodeIface>,
    ) -> i32
    where
        TargetNodeIface: IAutonomousNode,
    {
        error_codes::OK()
    }

    fn pass_object_to_connected<ObjectIface>(
        &self,
        _connection_index: i32,
        _object: canopy_rpc::SharedPtr<ObjectIface>,
    ) -> i32
    where
        ObjectIface: ISharedObject,
    {
        error_codes::OK()
    }

    fn receive_object<ObjectIface>(
        &self,
        _object: canopy_rpc::SharedPtr<ObjectIface>,
        _sender_node_id: u64,
    ) -> i32
    where
        ObjectIface: ISharedObject,
    {
        error_codes::OK()
    }

    fn get_node_status(
        &self,
        current_type: &mut NodeType,
        current_id: &mut u64,
        connections_count: &mut i32,
        objects_held: &mut i32,
    ) -> i32 {
        *current_type = NodeType::RootNode;
        *current_id = 0;
        *connections_count = 0;
        *objects_held = 0;
        error_codes::OK()
    }

    fn create_child_node(
        &self,
        _child_type: NodeType,
        _child_zone_id: u64,
        _cache_locally: bool,
        child_node: &mut canopy_rpc::SharedPtr<RustFuzzNode>,
    ) -> i32 {
        *child_node = canopy_rpc::Shared::null();
        error_codes::OK()
    }

    fn request_child_creation<TargetParentIface>(
        &self,
        _target_parent: canopy_rpc::SharedPtr<TargetParentIface>,
        _child_type: NodeType,
        _child_zone_id: u64,
        child_proxy: &mut canopy_rpc::SharedPtr<RustFuzzNode>,
    ) -> i32
    where
        TargetParentIface: IAutonomousNode,
    {
        *child_proxy = canopy_rpc::Shared::null();
        error_codes::OK()
    }

    fn get_cached_children_count(&self, count: &mut i32) -> i32 {
        *count = 0;
        error_codes::OK()
    }

    fn get_cached_child_by_index(
        &self,
        _index: i32,
        child: &mut canopy_rpc::SharedPtr<RustFuzzNode>,
    ) -> i32 {
        *child = canopy_rpc::Shared::null();
        error_codes::OK()
    }

    fn get_parent_node(&self, parent: &mut canopy_rpc::SharedPtr<RustFuzzNode>) -> i32 {
        *parent = canopy_rpc::Shared::null();
        error_codes::OK()
    }

    fn set_parent_node<ParentIface>(&self, _parent: canopy_rpc::SharedPtr<ParentIface>) -> i32
    where
        ParentIface: IAutonomousNode,
    {
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

    fn encode_create_child_node_ok(&self) -> SendResult {
        let mut response =
            canopy_protobuf_runtime_probe::__canopy_protobuf::fuzz_test::i_autonomous_node_create_child_nodeResponse::new();
        response.set_child_node(Self::proto_remote_object(&self.output_object));
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
                return Some(self.encode_create_child_node_ok());
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
            RustFuzzMarshaller {
                node: RustFuzzNode,
                service,
                output_object: output_object.clone(),
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
