//! Protocol Buffers specific support for the Rust Canopy runtime.
//!
//! Keep protobuf-facing metadata and helper APIs here so wire-format concerns
//! do not bleed into `internal/`.

pub mod facade;
pub mod metadata;

pub use facade::GeneratedProtobufCodec;
pub use facade::GeneratedProtobufMethodCodec;
pub use facade::ProtobufWireMessage;
pub use facade::UnsupportedGeneratedMessage;
pub use facade::call_generated_proxy_method;
pub use facade::deserialize_bytes;
pub use facade::deserialize_integer_vector;
pub use facade::deserialize_signed_bytes;
pub use facade::dispatch_generated_stub_call;
pub use facade::parse_generated_message;
pub use facade::serialize_bytes;
pub use facade::serialize_generated_message;
pub use facade::serialize_integer_vector;
pub use facade::serialize_signed_bytes;
pub use metadata::GeneratedProtobufBindingMetadata;
pub use metadata::GeneratedProtobufFieldKind;
pub use metadata::GeneratedProtobufMethodDescriptor;
pub use metadata::GeneratedProtobufParamDescriptor;
