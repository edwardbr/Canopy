//! Protocol Buffers specific generated metadata for Rust Canopy bindings.

use crate::internal::ParameterDirection;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GeneratedProtobufFieldKind {
    Scalar,
    Bytes,
    RepeatedScalar,
    MapScalar,
    InterfaceRemoteObject,
    PointerAddress,
    Enum,
    Message,
}

/// Distinguishes rpc::shared_ptr<T> from rpc::optimistic_ptr<T> in generated
/// protobuf parameter descriptors. Only meaningful for InterfaceRemoteObject fields.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InterfacePointerKind {
    Shared,
    Optimistic,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedProtobufParamDescriptor {
    pub name: &'static str,
    pub field_number: u32,
    pub direction: ParameterDirection,
    pub proto_type: &'static str,
    pub field_kind: GeneratedProtobufFieldKind,
    /// Pointer kind for interface parameters (Shared or Optimistic).
    /// None for non-interface parameter types.
    pub pointer_kind: Option<InterfacePointerKind>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedProtobufMethodDescriptor {
    pub method_name: &'static str,
    pub method_id: u64,
    pub schema_proto_file: &'static str,
    pub proto_package: &'static str,
    pub request_message: &'static str,
    pub response_message: &'static str,
    pub request_proto_full_name: &'static str,
    pub response_proto_full_name: &'static str,
    pub request_rust_type_name: &'static str,
    pub response_rust_type_name: &'static str,
    pub params: &'static [GeneratedProtobufParamDescriptor],
}

pub trait GeneratedProtobufBindingMetadata {
    fn protobuf_methods() -> &'static [GeneratedProtobufMethodDescriptor];

    fn protobuf_by_method_id(method_id: u64) -> Option<&'static GeneratedProtobufMethodDescriptor> {
        Self::protobuf_methods()
            .iter()
            .find(|descriptor| descriptor.method_id == method_id)
    }
}

#[cfg(test)]
mod tests {
    use super::{
        GeneratedProtobufBindingMetadata, GeneratedProtobufFieldKind,
        GeneratedProtobufMethodDescriptor, GeneratedProtobufParamDescriptor, InterfacePointerKind,
    };
    use crate::internal::ParameterDirection;

    struct ExampleProtobufBindingMetadata;

    impl GeneratedProtobufBindingMetadata for ExampleProtobufBindingMetadata {
        fn protobuf_methods() -> &'static [GeneratedProtobufMethodDescriptor] {
            static PARAMS: [GeneratedProtobufParamDescriptor; 1] =
                [GeneratedProtobufParamDescriptor {
                    name: "value",
                    field_number: 1,
                    direction: ParameterDirection::Out,
                    proto_type: "rpc.remote_object",
                    field_kind: GeneratedProtobufFieldKind::InterfaceRemoteObject,
                    pointer_kind: Some(InterfacePointerKind::Shared),
                }];
            static METHODS: [GeneratedProtobufMethodDescriptor; 1] =
                [GeneratedProtobufMethodDescriptor {
                    method_name: "foo",
                    method_id: 7,
                    schema_proto_file: "example/protobuf/schema/example.proto",
                    proto_package: "protobuf.example",
                    request_message: "example_i_demo_fooRequest",
                    response_message: "example_i_demo_fooResponse",
                    request_proto_full_name: "protobuf.example.example_i_demo_fooRequest",
                    response_proto_full_name: "protobuf.example.example_i_demo_fooResponse",
                    request_rust_type_name: "example_i_demo_fooRequest",
                    response_rust_type_name: "example_i_demo_fooResponse",
                    params: &PARAMS,
                }];
            &METHODS
        }
    }

    #[test]
    fn protobuf_binding_metadata_lookup_works() {
        let proto =
            ExampleProtobufBindingMetadata::protobuf_by_method_id(7).expect("protobuf method");
        assert_eq!(proto.request_message, "example_i_demo_fooRequest");
        assert_eq!(proto.proto_package, "protobuf.example");
        assert_eq!(
            proto.request_proto_full_name,
            "protobuf.example.example_i_demo_fooRequest"
        );
        assert_eq!(proto.request_rust_type_name, "example_i_demo_fooRequest");
        assert_eq!(
            proto.params[0].field_kind,
            GeneratedProtobufFieldKind::InterfaceRemoteObject
        );
        assert_eq!(
            proto.params[0].pointer_kind,
            Some(InterfacePointerKind::Shared)
        );
    }
}
