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
}

pub mod basic_rpc_probe {
    include!(env!("CANOPY_BASIC_RPC_BINDINGS_RS"));
}

pub mod basic_rpc_probe_protobuf {
    include!(env!("CANOPY_BASIC_RPC_PROTOBUF_BINDINGS_RS"));
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use crate::basic_rpc_probe::probe::i_math;
    use crate::basic_rpc_probe::probe::i_math::Interface;
    use crate::basic_rpc_probe::probe::i_peer;

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

    struct MathImpl;

    struct PeerImpl;

    impl canopy_rpc::CreateLocalProxy for PeerImpl {}

    impl canopy_rpc::CastingInterface for PeerImpl {
        fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
            i_peer::matches_interface_id(interface_id)
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

    impl i_peer::Interface for PeerImpl {
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

    impl i_math::Interface for MathImpl {
        type AcceptSharedPeerPeerIface0 = PeerImpl;
        type AcceptOptimisticPeerPeerIface0 = PeerImpl;

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
            p: crate::basic_rpc_probe::probe::point::Value,
            dx: i32,
            dy: i32,
            translated: &mut crate::basic_rpc_probe::probe::point::Value,
        ) -> i32 {
            *translated = crate::basic_rpc_probe::probe::point::Value {
                x: p.x + dx,
                y: p.y + dy,
            };
            canopy_rpc::OK()
        }

        fn label_value(
            &self,
            input: crate::basic_rpc_probe::probe::labeled_value::Value,
            output: &mut crate::basic_rpc_probe::probe::labeled_value::Value,
        ) -> i32 {
            *output = crate::basic_rpc_probe::probe::labeled_value::Value {
                label: format!("[{}]", input.label),
                value: input.value * 2,
            };
            canopy_rpc::OK()
        }

        fn accept_shared_peer<PEERIface0>(
            &self,
            peer: canopy_rpc::Shared<Arc<PEERIface0>>,
            seen: &mut i32,
        ) -> i32
        where
            PEERIface0: i_peer::Interface,
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

        fn accept_optimistic_peer<PEERIface0>(
            &self,
            peer: canopy_rpc::Optimistic<canopy_rpc::LocalProxy<PEERIface0>>,
            seen: &mut i32,
        ) -> i32
        where
            PEERIface0: i_peer::Interface,
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
    }

    struct LocalGeneratedTransport {
        service: Arc<canopy_rpc::Service>,
        context: canopy_rpc::GeneratedRpcCallContext,
    }

    impl canopy_rpc::GeneratedRpcCaller for LocalGeneratedTransport {
        fn marshaller(&self) -> &dyn canopy_rpc::IMarshaller {
            self.service.as_ref()
        }

        fn call_context(&self) -> canopy_rpc::GeneratedRpcCallContext {
            self.context.clone()
        }
    }

    struct MathProxy {
        transport: LocalGeneratedTransport,
    }

    impl canopy_rpc::CreateLocalProxy for MathProxy {}

    impl canopy_rpc::CastingInterface for MathProxy {
        fn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool {
            i_math::matches_interface_id(interface_id)
        }
    }

    impl canopy_rpc::GeneratedRustInterface for MathProxy {
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

    impl i_math::Interface for MathProxy {
        type AcceptSharedPeerPeerIface0 = PeerImpl;
        type AcceptOptimisticPeerPeerIface0 = PeerImpl;

        fn add(&self, a: i32, b: i32, c: &mut i32) -> i32 {
            let request = i_math::interface_binding::add::Request::from_call(a, b);
            let response = <Self as i_math::Proxy>::proxy_call_add(self, request);
            response.apply_to_call(c)
        }

        fn bounce_text(&self, input: String, output: &mut String) -> i32 {
            let request = i_math::interface_binding::bounce_text::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_bounce_text(self, request);
            response.apply_to_call(output)
        }

        fn reverse_sequence(&self, input: Vec<u64>, output: &mut Vec<u64>) -> i32 {
            let request = i_math::interface_binding::reverse_sequence::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_reverse_sequence(self, request);
            response.apply_to_call(output)
        }

        fn increment_sequence(&self, input: Vec<i32>, output: &mut Vec<i32>) -> i32 {
            let request = i_math::interface_binding::increment_sequence::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_increment_sequence(self, request);
            response.apply_to_call(output)
        }

        fn decorate_many(&self, input: Vec<String>, output: &mut Vec<String>) -> i32 {
            let request = i_math::interface_binding::decorate_many::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_decorate_many(self, request);
            response.apply_to_call(output)
        }

        fn echo_bytes(&self, input: Vec<u8>, output: &mut Vec<u8>) -> i32 {
            let request = i_math::interface_binding::echo_bytes::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_echo_bytes(self, request);
            response.apply_to_call(output)
        }

        fn negate_signed_bytes(&self, input: Vec<i8>, output: &mut Vec<i8>) -> i32 {
            let request = i_math::interface_binding::negate_signed_bytes::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_negate_signed_bytes(self, request);
            response.apply_to_call(output)
        }

        fn translate_point(
            &self,
            p: crate::basic_rpc_probe::probe::point::Value,
            dx: i32,
            dy: i32,
            translated: &mut crate::basic_rpc_probe::probe::point::Value,
        ) -> i32 {
            let request = i_math::interface_binding::translate_point::Request::from_call(p, dx, dy);
            let response = <Self as i_math::Proxy>::proxy_call_translate_point(self, request);
            response.apply_to_call(translated)
        }

        fn label_value(
            &self,
            input: crate::basic_rpc_probe::probe::labeled_value::Value,
            output: &mut crate::basic_rpc_probe::probe::labeled_value::Value,
        ) -> i32 {
            let request = i_math::interface_binding::label_value::Request::from_call(input);
            let response = <Self as i_math::Proxy>::proxy_call_label_value(self, request);
            response.apply_to_call(output)
        }

        fn accept_shared_peer<PEERIface0>(
            &self,
            peer: canopy_rpc::Shared<Arc<PEERIface0>>,
            seen: &mut i32,
        ) -> i32
        where
            PEERIface0: i_peer::Interface,
        {
            let _ = peer;
            *seen = canopy_rpc::INVALID_DATA();
            canopy_rpc::INVALID_DATA()
        }

        fn accept_optimistic_peer<PEERIface0>(
            &self,
            peer: canopy_rpc::Optimistic<canopy_rpc::LocalProxy<PEERIface0>>,
            seen: &mut i32,
        ) -> i32
        where
            PEERIface0: i_peer::Interface,
        {
            let _ = peer;
            *seen = canopy_rpc::INVALID_DATA();
            canopy_rpc::INVALID_DATA()
        }
    }

    impl i_math::Proxy for MathProxy {
        fn proxy_caller(&self) -> Option<&dyn canopy_rpc::GeneratedRpcCaller> {
            Some(&self.transport)
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

        let proxy = MathProxy {
            transport: LocalGeneratedTransport {
                service: service.clone(),
                context: canopy_rpc::GeneratedRpcCallContext {
                    protocol_version: canopy_rpc::get_version(),
                    encoding_type: canopy_rpc::Encoding::ProtocolBuffers,
                    tag: 0,
                    caller_zone_id,
                    remote_object_id: remote_object.clone(),
                },
            },
        };

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

        let p = crate::basic_rpc_probe::probe::point::Value { x: 3, y: 7 };
        let mut translated = crate::basic_rpc_probe::probe::point::Value::default();
        let translate_result = proxy.translate_point(p, 10, -2, &mut translated);

        assert_eq!(translate_result, canopy_rpc::OK());
        assert_eq!(translated.x, 13);
        assert_eq!(translated.y, 5);

        let lv = crate::basic_rpc_probe::probe::labeled_value::Value {
            label: "hello".to_string(),
            value: 21,
        };
        let mut lv_out = crate::basic_rpc_probe::probe::labeled_value::Value::default();
        let label_result = proxy.label_value(lv, &mut lv_out);

        assert_eq!(label_result, canopy_rpc::OK());
        assert_eq!(lv_out.label, "[hello]");
        assert_eq!(lv_out.value, 42);

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
}
