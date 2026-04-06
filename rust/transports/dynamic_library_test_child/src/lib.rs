//! Handwritten Rust child DLL for the dynamic-library transport.

mod exports;

use canopy_protobuf_runtime_probe::basic_rpc_probe::probe::{i_math, i_peer};
use canopy_rpc::internal::error_codes;
use canopy_rpc::{
    AddRefParams, AddressType, DefaultValues, GetNewZoneIdParams, NewZoneIdResult, Object,
    ObjectReleasedParams, PostParams, ReleaseParams, SendParams, SendResult, StandardResult,
    TransportDownParams, TryCastParams, Zone, ZoneAddress, ZoneAddressArgs,
};
use canopy_transport_dynamic_library::ParentCallbacks;
use canopy_transport_dynamic_library::ParentTransportAdapter;
use std::sync::{Arc, Mutex};

fn sample_zone_address() -> canopy_rpc::ZoneAddress {
    ZoneAddress::create(ZoneAddressArgs::new(
        DefaultValues::VERSION_3,
        AddressType::Ipv4,
        31337,
        vec![127, 0, 0, 1],
        32,
        7,
        16,
        0,
        vec![],
    ))
    .expect("sample zone address should be valid")
}

fn sample_zone() -> canopy_rpc::Zone {
    Zone::new(sample_zone_address())
}

fn sample_output_object() -> canopy_rpc::RemoteObject {
    sample_zone()
        .with_object(Object::new(100))
        .expect("with_object should succeed")
}

#[derive(Default)]
struct Stats {
    post_count: usize,
    add_ref_count: usize,
    release_count: usize,
    object_released_count: usize,
    transport_down_count: usize,
}

#[derive(Clone)]
struct TestChildMarshaller {
    stats: Arc<Mutex<Stats>>,
    parent_transport: ParentTransportAdapter<ParentCallbacks>,
    generated_math: GeneratedMathObject,
}

impl TestChildMarshaller {
    fn new(parent_transport: ParentTransportAdapter<ParentCallbacks>) -> Self {
        Self {
            stats: Arc::default(),
            parent_transport,
            generated_math: GeneratedMathObject,
        }
    }

    fn stats_string(&self) -> Vec<u8> {
        let stats = self
            .stats
            .lock()
            .expect("stats mutex should not be poisoned");
        format!(
            "post={};add_ref={};release={};object_released={};transport_down={}",
            stats.post_count,
            stats.add_ref_count,
            stats.release_count,
            stats.object_released_count,
            stats.transport_down_count
        )
        .into_bytes()
    }

    fn call_parent_send(&self) -> SendResult {
        let caller_zone = sample_zone();
        let remote_object = sample_output_object();
        self.parent_transport.send(SendParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: canopy_rpc::Encoding::ProtocolBuffers,
            tag: 909,
            caller_zone_id: caller_zone,
            remote_object_id: remote_object,
            interface_id: canopy_rpc::InterfaceOrdinal::new(41),
            method_id: canopy_rpc::Method::new(42),
            in_data: b"from-rust-child".to_vec(),
            in_back_channel: Vec::new(),
        })
    }

    fn call_parent_get_new_zone_id(&self) -> SendResult {
        let result = self.parent_transport.get_new_zone_id(GetNewZoneIdParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            in_back_channel: Vec::new(),
        });

        if result.error_code != error_codes::OK() {
            return SendResult::new(result.error_code, Vec::new(), Vec::new());
        }

        SendResult::new(
            error_codes::OK(),
            format!("zone:{}", result.zone_id.get_subnet()).into_bytes(),
            Vec::new(),
        )
    }

    fn parent_test_zone(&self) -> Zone {
        sample_zone()
    }

    fn parent_test_remote_object(&self) -> canopy_rpc::RemoteObject {
        sample_output_object()
    }

    fn call_parent_post(&self) -> SendResult {
        self.parent_transport.post(PostParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            encoding_type: canopy_rpc::Encoding::ProtocolBuffers,
            tag: 910,
            caller_zone_id: self.parent_test_zone(),
            remote_object_id: self.parent_test_remote_object(),
            interface_id: canopy_rpc::InterfaceOrdinal::new(43),
            method_id: canopy_rpc::Method::new(44),
            in_data: b"post-from-rust-child".to_vec(),
            in_back_channel: Vec::new(),
        });
        SendResult::new(error_codes::OK(), b"post-ok".to_vec(), Vec::new())
    }

    fn call_parent_try_cast(&self) -> SendResult {
        let result = self.parent_transport.try_cast(TryCastParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            caller_zone_id: self.parent_test_zone(),
            remote_object_id: self.parent_test_remote_object(),
            interface_id: canopy_rpc::InterfaceOrdinal::new(45),
            in_back_channel: Vec::new(),
        });
        SendResult::new(
            result.error_code,
            format!("try-cast:{}", result.error_code).into_bytes(),
            Vec::new(),
        )
    }

    fn call_parent_add_ref(&self) -> SendResult {
        let result = self.parent_transport.add_ref(AddRefParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            remote_object_id: self.parent_test_remote_object(),
            caller_zone_id: self.parent_test_zone(),
            requesting_zone_id: self.parent_test_zone(),
            build_out_param_channel: canopy_rpc::AddRefOptions::NORMAL,
            in_back_channel: Vec::new(),
        });
        SendResult::new(
            result.error_code,
            format!("add-ref:{}", result.error_code).into_bytes(),
            Vec::new(),
        )
    }

    fn call_parent_release(&self) -> SendResult {
        let result = self.parent_transport.release(ReleaseParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            remote_object_id: self.parent_test_remote_object(),
            caller_zone_id: self.parent_test_zone(),
            options: canopy_rpc::ReleaseOptions::NORMAL,
            in_back_channel: Vec::new(),
        });
        SendResult::new(
            result.error_code,
            format!("release:{}", result.error_code).into_bytes(),
            Vec::new(),
        )
    }

    fn call_parent_object_released(&self) -> SendResult {
        self.parent_transport.object_released(ObjectReleasedParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            remote_object_id: self.parent_test_remote_object(),
            caller_zone_id: self.parent_test_zone(),
            in_back_channel: Vec::new(),
        });
        SendResult::new(
            error_codes::OK(),
            b"object-released-ok".to_vec(),
            Vec::new(),
        )
    }

    fn call_parent_transport_down(&self) -> SendResult {
        self.parent_transport.transport_down(TransportDownParams {
            protocol_version: DefaultValues::VERSION_3 as u64,
            destination_zone_id: self.parent_test_zone(),
            caller_zone_id: self.parent_test_zone(),
            in_back_channel: Vec::new(),
        });
        SendResult::new(error_codes::OK(), b"transport-down-ok".to_vec(), Vec::new())
    }

    fn dispatch_generated_probe_call(&self, params: SendParams) -> Option<SendResult> {
        if params.encoding_type != canopy_rpc::Encoding::ProtocolBuffers {
            return None;
        }

        if params.interface_id != canopy_rpc::InterfaceOrdinal::new(i_math::ID_RPC_V3)
            && params.interface_id != canopy_rpc::InterfaceOrdinal::new(i_math::ID_RPC_V2)
        {
            return None;
        }

        let context = canopy_rpc::DispatchContext::from(&params);
        Some(i_math::Interface::__rpc_dispatch_generated(
            &self.generated_math,
            &context,
            params,
        ))
    }
}

#[derive(Clone)]
struct GeneratedMathObject;

impl canopy_rpc::CreateLocalProxy for GeneratedMathObject {}

impl canopy_rpc::CastingInterface for GeneratedMathObject {
    fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
        i_math::matches_interface_id(interface_id)
    }
}

impl canopy_rpc::GeneratedRustInterface for GeneratedMathObject {
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

impl i_math::Interface for GeneratedMathObject {
    type AcceptSharedPeerPeerIface0 = i_peer::ProxySkeleton;
    type AcceptOptimisticPeerPeerIface0 = i_peer::ProxySkeleton;

    fn add(&self, a: i32, b: i32, c: &mut i32) -> i32 {
        *c = a + b;
        canopy_rpc::OK()
    }

    fn bounce_text(&self, input: String, output: &mut String) -> i32 {
        *output = format!("rust-child:{input}");
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
        p: canopy_protobuf_runtime_probe::basic_rpc_probe::probe::point::Value,
        dx: i32,
        dy: i32,
        translated: &mut canopy_protobuf_runtime_probe::basic_rpc_probe::probe::point::Value,
    ) -> i32 {
        translated.x = p.x + dx;
        translated.y = p.y + dy;
        canopy_rpc::OK()
    }

    fn label_value(
        &self,
        input: canopy_protobuf_runtime_probe::basic_rpc_probe::probe::labeled_value::Value,
        output: &mut canopy_protobuf_runtime_probe::basic_rpc_probe::probe::labeled_value::Value,
    ) -> i32 {
        output.label = format!("[{}]", input.label);
        output.value = input.value * 2;
        canopy_rpc::OK()
    }

    fn accept_shared_peer<PEERIface0>(
        &self,
        _peer: canopy_rpc::Shared<Arc<PEERIface0>>,
        seen: &mut i32,
    ) -> i32
    where
        PEERIface0: i_peer::Interface,
    {
        *seen = canopy_rpc::INVALID_DATA();
        canopy_rpc::INVALID_DATA()
    }

    fn accept_optimistic_peer<PEERIface0>(
        &self,
        _peer: canopy_rpc::Optimistic<canopy_rpc::LocalProxy<PEERIface0>>,
        seen: &mut i32,
    ) -> i32
    where
        PEERIface0: i_peer::Interface,
    {
        *seen = canopy_rpc::INVALID_DATA();
        canopy_rpc::INVALID_DATA()
    }
}

impl canopy_rpc::IMarshaller for TestChildMarshaller {
    fn send(&self, params: SendParams) -> SendResult {
        if let Some(result) = self.dispatch_generated_probe_call(params.clone()) {
            return result;
        }

        if params.in_data == b"call-parent-send" {
            return self.call_parent_send();
        }

        if params.in_data == b"call-parent-get-new-zone-id" {
            return self.call_parent_get_new_zone_id();
        }

        if params.in_data == b"call-parent-post" {
            return self.call_parent_post();
        }

        if params.in_data == b"call-parent-try-cast" {
            return self.call_parent_try_cast();
        }

        if params.in_data == b"call-parent-add-ref" {
            return self.call_parent_add_ref();
        }

        if params.in_data == b"call-parent-release" {
            return self.call_parent_release();
        }

        if params.in_data == b"call-parent-object-released" {
            return self.call_parent_object_released();
        }

        if params.in_data == b"call-parent-transport-down" {
            return self.call_parent_transport_down();
        }

        if params.in_data == b"stats" {
            return SendResult::new(error_codes::OK(), self.stats_string(), Vec::new());
        }

        let mut out = b"rust-child:".to_vec();
        out.extend_from_slice(&params.in_data);
        SendResult::new(error_codes::OK(), out, Vec::new())
    }

    fn post(&self, _params: PostParams) {
        let mut stats = self
            .stats
            .lock()
            .expect("stats mutex should not be poisoned");
        stats.post_count += 1;
    }

    fn try_cast(&self, _params: TryCastParams) -> StandardResult {
        StandardResult::new(error_codes::OK(), Vec::new())
    }

    fn add_ref(&self, _params: AddRefParams) -> StandardResult {
        let mut stats = self
            .stats
            .lock()
            .expect("stats mutex should not be poisoned");
        stats.add_ref_count += 1;
        StandardResult::new(error_codes::OK(), Vec::new())
    }

    fn release(&self, _params: ReleaseParams) -> StandardResult {
        let mut stats = self
            .stats
            .lock()
            .expect("stats mutex should not be poisoned");
        stats.release_count += 1;
        StandardResult::new(error_codes::OK(), Vec::new())
    }

    fn object_released(&self, _params: ObjectReleasedParams) {
        let mut stats = self
            .stats
            .lock()
            .expect("stats mutex should not be poisoned");
        stats.object_released_count += 1;
    }

    fn transport_down(&self, _params: TransportDownParams) {
        let mut stats = self
            .stats
            .lock()
            .expect("stats mutex should not be poisoned");
        stats.transport_down_count += 1;
    }

    fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
        NewZoneIdResult::new(error_codes::OK(), sample_zone(), Vec::new())
    }
}
