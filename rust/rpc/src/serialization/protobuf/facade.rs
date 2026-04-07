//! Format-specific helper facade for Protocol Buffers support.
//!
//! This is the Rust counterpart of `c++/rpc/include/rpc/serialization/protobuf/protobuf.h`.
//! It is intentionally limited to protobuf-facing data-shape helpers and codec
//! traits; generic runtime logic should not depend on these details.

use super::metadata::GeneratedProtobufMethodDescriptor;

/// Converts a byte-oriented Rust container into protobuf `bytes` payload data.
pub fn serialize_bytes<T>(data: T) -> Vec<u8>
where
    T: AsRef<[u8]>,
{
    data.as_ref().to_vec()
}

/// Converts protobuf `bytes` payload data back into Rust-owned bytes.
pub fn deserialize_bytes<T>(proto_bytes: T) -> Vec<u8>
where
    T: AsRef<[u8]>,
{
    proto_bytes.as_ref().to_vec()
}

/// Converts signed byte containers into protobuf `bytes` payload data.
pub fn serialize_signed_bytes(data: &[i8]) -> Vec<u8> {
    data.iter().map(|value| *value as u8).collect()
}

/// Converts protobuf `bytes` payload data back into signed bytes.
pub fn deserialize_signed_bytes<T>(proto_bytes: T) -> Vec<i8>
where
    T: AsRef<[u8]>,
{
    proto_bytes
        .as_ref()
        .iter()
        .map(|value| *value as i8)
        .collect()
}

/// Converts an integer vector into a protobuf repeated-field-friendly vector.
pub fn serialize_integer_vector<T>(data: &[T]) -> Vec<T>
where
    T: Copy,
{
    data.to_vec()
}

/// Converts a protobuf repeated-field-friendly vector back into a Rust vector.
pub fn deserialize_integer_vector<T>(proto_field: &[T]) -> Vec<T>
where
    T: Copy,
{
    proto_field.to_vec()
}

/// Future home for generated protobuf message conversion.
///
/// This trait deliberately lives in the protobuf facade so generated or
/// handwritten request/response conversion stays format-specific.
pub trait GeneratedProtobufCodec<Message>: Sized {
    fn to_protobuf_message(&self) -> Result<Message, i32>;

    fn from_protobuf_message(message: Message) -> Result<Self, i32>;
}

/// Minimal format-specific wire-message seam for generated protobuf dispatch.
///
/// This stays inside the protobuf facade so generic runtime code does not
/// depend on a particular protobuf runtime crate. Future integration with the
/// in-tree protobuf Rust runtime can implement this for the real generated
/// message types.
pub trait ProtobufWireMessage: Sized {
    fn parse_from_bytes(proto_bytes: &[u8]) -> Result<Self, i32>;

    fn serialize_to_bytes(&self) -> Result<Vec<u8>, i32>;
}

pub fn parse_generated_message<Message>(proto_bytes: &[u8]) -> Result<Message, i32>
where
    Message: ProtobufWireMessage,
{
    Message::parse_from_bytes(proto_bytes)
}

pub fn serialize_generated_message<Message>(message: &Message) -> Result<Vec<u8>, i32>
where
    Message: ProtobufWireMessage,
{
    message.serialize_to_bytes()
}

#[cfg(feature = "protobuf-runtime")]
impl<T> ProtobufWireMessage for T
where
    T: protobuf::Message + protobuf::Parse + protobuf::Serialize,
{
    fn parse_from_bytes(proto_bytes: &[u8]) -> Result<Self, i32> {
        <T as protobuf::Parse>::parse(proto_bytes).map_err(|_| crate::INVALID_DATA())
    }

    fn serialize_to_bytes(&self) -> Result<Vec<u8>, i32> {
        <T as protobuf::Serialize>::serialize(self).map_err(|_| crate::INVALID_DATA())
    }
}

/// Placeholder protobuf message type used until the real generated Rust
/// protobuf module is wired into the build.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct UnsupportedGeneratedMessage;

impl ProtobufWireMessage for UnsupportedGeneratedMessage {
    fn parse_from_bytes(_proto_bytes: &[u8]) -> Result<Self, i32> {
        Err(crate::INVALID_DATA())
    }

    fn serialize_to_bytes(&self) -> Result<Vec<u8>, i32> {
        Err(crate::INVALID_DATA())
    }
}

/// Per-method protobuf codec seam for generated request/response shapes.
///
/// The generic Rust generator owns the request/response structs. The protobuf
/// generator owns the format-specific codec companion that knows which method
/// metadata applies and, later, how to decode and encode the canonical protobuf
/// bytes for that method.
pub trait GeneratedProtobufMethodCodec {
    type ProxyRequest;
    type ProxyResponse;
    type DispatchRequest;
    type DispatchResponse;
    type ProtoRequest: ProtobufWireMessage;
    type ProtoResponse: ProtobufWireMessage;

    fn descriptor() -> &'static GeneratedProtobufMethodDescriptor;

    fn request_from_protobuf_message(
        _message: Self::ProtoRequest,
    ) -> Result<Self::DispatchRequest, i32> {
        Err(crate::INVALID_DATA())
    }

    fn response_to_protobuf_message(
        _response: &Self::DispatchResponse,
    ) -> Result<Self::ProtoResponse, i32> {
        Err(crate::INVALID_DATA())
    }

    fn request_to_protobuf_message(
        _request: &Self::ProxyRequest,
    ) -> Result<Self::ProtoRequest, i32> {
        Err(crate::INVALID_DATA())
    }

    fn request_to_protobuf_message_with_caller(
        _caller: &dyn crate::GeneratedRpcCaller,
        request: &Self::ProxyRequest,
    ) -> Result<Self::ProtoRequest, i32> {
        Self::request_to_protobuf_message(request)
    }

    fn response_from_protobuf_message(
        _message: Self::ProtoResponse,
    ) -> Result<Self::ProxyResponse, i32> {
        Err(crate::INVALID_DATA())
    }

    fn response_from_protobuf_message_with_caller(
        _caller: &dyn crate::GeneratedRpcCaller,
        message: Self::ProtoResponse,
    ) -> Result<Self::ProxyResponse, i32> {
        Self::response_from_protobuf_message(message)
    }

    fn decode_request(proto_bytes: &[u8]) -> Result<Self::DispatchRequest, i32> {
        let message = Self::ProtoRequest::parse_from_bytes(proto_bytes)?;
        Self::request_from_protobuf_message(message)
    }

    fn encode_request(request: &Self::ProxyRequest) -> Result<Vec<u8>, i32> {
        let message = Self::request_to_protobuf_message(request)?;
        message.serialize_to_bytes()
    }

    fn encode_request_with_caller(
        caller: &dyn crate::GeneratedRpcCaller,
        request: &Self::ProxyRequest,
    ) -> Result<Vec<u8>, i32> {
        let message = Self::request_to_protobuf_message_with_caller(caller, request)?;
        message.serialize_to_bytes()
    }

    fn encode_response(response: &Self::DispatchResponse) -> Result<Vec<u8>, i32> {
        let message = Self::response_to_protobuf_message(response)?;
        message.serialize_to_bytes()
    }

    fn decode_response(proto_bytes: &[u8]) -> Result<Self::ProxyResponse, i32> {
        let message = Self::ProtoResponse::parse_from_bytes(proto_bytes)?;
        Self::response_from_protobuf_message(message)
    }

    fn decode_response_with_caller(
        caller: &dyn crate::GeneratedRpcCaller,
        proto_bytes: &[u8],
    ) -> Result<Self::ProxyResponse, i32> {
        let message = Self::ProtoResponse::parse_from_bytes(proto_bytes)?;
        Self::response_from_protobuf_message_with_caller(caller, message)
    }
}

/// Central protobuf stub-dispatch seam for generated Rust.
///
/// This keeps protobuf-specific request decode / response encode flow inside the
/// protobuf facade rather than duplicating it in generated modules.
pub fn dispatch_generated_stub_call<Codec, DispatchFn>(
    params: crate::SendParams,
    dispatch: DispatchFn,
) -> crate::SendResult
where
    Codec: GeneratedProtobufMethodCodec,
    DispatchFn: FnOnce(Codec::DispatchRequest) -> Result<Codec::DispatchResponse, i32>,
{
    let request = match Codec::decode_request(&params.in_data) {
        Ok(request) => request,
        Err(err) => return crate::SendResult::new(err, vec![], vec![]),
    };

    let response = match dispatch(request) {
        Ok(response) => response,
        Err(err) => return crate::SendResult::new(err, vec![], vec![]),
    };

    match Codec::encode_response(&response) {
        Ok(out_buf) => crate::SendResult::new(crate::OK(), out_buf, vec![]),
        Err(err) => crate::SendResult::new(err, vec![], vec![]),
    }
}

pub fn call_generated_proxy_method<Codec, FromError, TransportError>(
    caller: Option<&dyn crate::GeneratedRpcCaller>,
    interface_id: crate::InterfaceOrdinal,
    method_id: crate::Method,
    request: &Codec::ProxyRequest,
    from_error_code: FromError,
    transport_error: TransportError,
) -> Codec::ProxyResponse
where
    Codec: GeneratedProtobufMethodCodec,
    FromError: Fn(i32) -> Codec::ProxyResponse,
    TransportError: FnOnce() -> Codec::ProxyResponse,
{
    let Some(caller) = caller else {
        return transport_error();
    };

    let in_data = match Codec::encode_request_with_caller(caller, request) {
        Ok(in_data) => in_data,
        Err(error_code) => return from_error_code(error_code),
    };

    let result = caller.send_generated(interface_id, method_id, in_data, vec![]);
    if crate::is_error(result.error_code) {
        return from_error_code(result.error_code);
    }

    match Codec::decode_response_with_caller(caller, &result.out_buf) {
        Ok(response) => response,
        Err(error_code) => from_error_code(error_code),
    }
}

#[cfg(test)]
mod tests {
    use super::{
        GeneratedProtobufCodec, GeneratedProtobufMethodCodec, ProtobufWireMessage,
        UnsupportedGeneratedMessage, deserialize_bytes, deserialize_integer_vector,
        deserialize_signed_bytes, dispatch_generated_stub_call, serialize_bytes,
        serialize_integer_vector, serialize_signed_bytes,
    };
    use crate::ParameterDirection;
    use crate::serialization::protobuf::{
        GeneratedProtobufFieldKind, GeneratedProtobufMethodDescriptor,
        GeneratedProtobufParamDescriptor,
    };

    #[test]
    fn bytes_round_trip_matches_cpp_intent() {
        let encoded = serialize_bytes([1_u8, 2, 3]);
        assert_eq!(encoded, vec![1, 2, 3]);
        assert_eq!(deserialize_bytes(&encoded), vec![1, 2, 3]);
    }

    #[test]
    fn signed_bytes_round_trip_matches_cpp_intent() {
        let encoded = serialize_signed_bytes(&[-1_i8, 0, 1]);
        assert_eq!(encoded, vec![255, 0, 1]);
        assert_eq!(deserialize_signed_bytes(&encoded), vec![-1, 0, 1]);
    }

    #[test]
    fn integer_vector_helpers_clone_repeated_field_shape() {
        let encoded = serialize_integer_vector(&[10_u64, 20_u64, 30_u64]);
        assert_eq!(encoded, vec![10, 20, 30]);
        assert_eq!(deserialize_integer_vector(&encoded), vec![10, 20, 30]);
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    struct ExampleCodec(i32);

    impl GeneratedProtobufCodec<i32> for ExampleCodec {
        fn to_protobuf_message(&self) -> Result<i32, i32> {
            Ok(self.0)
        }

        fn from_protobuf_message(message: i32) -> Result<Self, i32> {
            Ok(Self(message))
        }
    }

    #[test]
    fn generated_protobuf_codec_trait_is_usable_without_runtime_contamination() {
        let message = ExampleCodec(42).to_protobuf_message().expect("encode");
        let decoded = ExampleCodec::from_protobuf_message(message).expect("decode");
        assert_eq!(decoded, ExampleCodec(42));
    }

    struct ExampleMethodCodec;

    impl GeneratedProtobufMethodCodec for ExampleMethodCodec {
        type ProxyRequest = ();
        type ProxyResponse = ();
        type DispatchRequest = ();
        type DispatchResponse = ();
        type ProtoRequest = UnsupportedGeneratedMessage;
        type ProtoResponse = UnsupportedGeneratedMessage;

        fn descriptor() -> &'static GeneratedProtobufMethodDescriptor {
            static PARAMS: [GeneratedProtobufParamDescriptor; 1] =
                [GeneratedProtobufParamDescriptor {
                    name: "value",
                    field_number: 1,
                    direction: ParameterDirection::In,
                    proto_type: "int32",
                    field_kind: GeneratedProtobufFieldKind::Scalar,
                    pointer_kind: None,
                }];
            static METHOD: GeneratedProtobufMethodDescriptor = GeneratedProtobufMethodDescriptor {
                method_name: "foo",
                method_id: 7,
                schema_proto_file: "example.proto",
                proto_package: "protobuf.example",
                request_message: "fooRequest",
                response_message: "fooResponse",
                request_proto_full_name: "protobuf.example.fooRequest",
                response_proto_full_name: "protobuf.example.fooResponse",
                request_rust_type_name: "fooRequest",
                response_rust_type_name: "fooResponse",
                params: &PARAMS,
            };
            &METHOD
        }
    }

    #[test]
    fn generated_protobuf_method_codec_trait_exposes_descriptor_without_runtime_leakage() {
        let descriptor = ExampleMethodCodec::descriptor();
        assert_eq!(descriptor.method_name, "foo");
        assert_eq!(
            descriptor.params[0].field_kind,
            GeneratedProtobufFieldKind::Scalar
        );
        assert_eq!(
            ExampleMethodCodec::decode_request(&[]),
            Err(crate::INVALID_DATA())
        );
        assert_eq!(
            ExampleMethodCodec::encode_response(&()),
            Err(crate::INVALID_DATA())
        );
    }

    struct ExampleDispatchCodec;

    #[derive(Debug, Clone, PartialEq, Eq)]
    struct ExampleWireMessage(i32);

    impl ProtobufWireMessage for ExampleWireMessage {
        fn parse_from_bytes(proto_bytes: &[u8]) -> Result<Self, i32> {
            if proto_bytes == [7] {
                Ok(Self(7))
            } else {
                Err(crate::INVALID_DATA())
            }
        }

        fn serialize_to_bytes(&self) -> Result<Vec<u8>, i32> {
            Ok(vec![self.0 as u8])
        }
    }

    impl GeneratedProtobufMethodCodec for ExampleDispatchCodec {
        type ProxyRequest = i32;
        type ProxyResponse = i32;
        type DispatchRequest = i32;
        type DispatchResponse = i32;
        type ProtoRequest = ExampleWireMessage;
        type ProtoResponse = ExampleWireMessage;

        fn descriptor() -> &'static GeneratedProtobufMethodDescriptor {
            ExampleMethodCodec::descriptor()
        }

        fn request_from_protobuf_message(
            message: Self::ProtoRequest,
        ) -> Result<Self::DispatchRequest, i32> {
            Ok(message.0)
        }

        fn response_to_protobuf_message(
            response: &Self::DispatchResponse,
        ) -> Result<Self::ProtoResponse, i32> {
            Ok(ExampleWireMessage(*response))
        }
    }

    #[test]
    fn generated_stub_dispatch_call_uses_codec_decode_and_encode() {
        let params = crate::SendParams {
            protocol_version: crate::get_version(),
            caller_zone_id: crate::CallerZone::default(),
            remote_object_id: crate::RemoteObject::default(),
            interface_id: crate::InterfaceOrdinal::new(3),
            method_id: crate::Method::new(7),
            encoding_type: crate::Encoding::ProtocolBuffers,
            tag: 0,
            in_data: vec![7],
            in_back_channel: vec![],
        };

        let result = dispatch_generated_stub_call::<ExampleDispatchCodec, _>(params, |request| {
            Ok(request + 1)
        });

        assert_eq!(result.error_code, crate::OK());
        assert_eq!(result.out_buf, vec![8]);
        assert!(result.out_back_channel.is_empty());
    }

    #[test]
    fn generated_stub_dispatch_call_returns_codec_errors_without_runtime_leakage() {
        let params = crate::SendParams {
            protocol_version: crate::get_version(),
            caller_zone_id: crate::CallerZone::default(),
            remote_object_id: crate::RemoteObject::default(),
            interface_id: crate::InterfaceOrdinal::new(3),
            method_id: crate::Method::new(7),
            encoding_type: crate::Encoding::ProtocolBuffers,
            tag: 0,
            in_data: vec![1],
            in_back_channel: vec![],
        };

        let result =
            dispatch_generated_stub_call::<ExampleDispatchCodec, _>(params, |_request| Ok(99));

        assert_eq!(result.error_code, crate::INVALID_DATA());
        assert!(result.out_buf.is_empty());
        assert!(result.out_back_channel.is_empty());
    }
}
