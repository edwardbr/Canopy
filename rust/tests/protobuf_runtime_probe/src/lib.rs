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

    fn zone(subnet: u64) -> canopy_rpc::Zone {
        let mut args = canopy_rpc::ZoneAddressArgs::default();
        args.subnet = subnet;
        canopy_rpc::Zone::from(canopy_rpc::ZoneAddress::create(args).expect("zone address"))
    }

    struct MathImpl;

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
                    remote_object_id: remote_object,
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
    }
}
