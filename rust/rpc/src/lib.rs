//! Handwritten Rust runtime for Canopy RPC.
//!
//! This crate will mirror the role of `c++/rpc` while preserving the
//! protocol contracts shared with the existing C++ implementation.

#[doc(hidden)]
pub mod internal;
pub mod rpc_types;
pub mod serialization;
pub mod version;

#[doc(hidden)]
pub use internal::AddRefParams;
#[doc(hidden)]
pub use internal::BindableInterfaceValue;
#[doc(hidden)]
pub use internal::BoundInterface;
#[doc(hidden)]
pub use internal::CastingInterface;
#[doc(hidden)]
pub use internal::ConnectResult;
#[doc(hidden)]
pub use internal::CreateLocalProxy;
#[doc(hidden)]
pub use internal::CreateRemoteProxy;
#[doc(hidden)]
pub use internal::CurrentServiceGuard;
#[doc(hidden)]
pub use internal::DUMMY_OBJECT_ID;
pub use internal::DispatchContext;
pub use internal::DispatchResult;
#[doc(hidden)]
pub use internal::GeneratedInterfaceBindingMetadata;
#[doc(hidden)]
pub use internal::GeneratedInterfaceParamDescriptor;
#[doc(hidden)]
pub use internal::GeneratedMethodBindingDescriptor;
#[doc(hidden)]
pub use internal::GeneratedRpcCallContext;
#[doc(hidden)]
pub use internal::GeneratedRpcCaller;
#[doc(hidden)]
pub use internal::GeneratedRustInterface;
#[doc(hidden)]
pub use internal::GetNewZoneIdParams;
#[doc(hidden)]
pub use internal::IMarshaller;
pub use internal::INCOMPATIBLE_SERIALISATION;
pub use internal::INVALID_DATA;
pub use internal::INVALID_INTERFACE_ID;
pub use internal::INVALID_METHOD_ID;
#[doc(hidden)]
pub use internal::InterfaceBindResult;
#[doc(hidden)]
pub use internal::InterfaceBindingOrigin;
#[doc(hidden)]
pub use internal::InterfacePointerKind;
#[doc(hidden)]
pub use internal::LocalObjectAdapter;
#[doc(hidden)]
pub use internal::LocalProxy;
#[doc(hidden)]
pub use internal::NewZoneIdResult;
pub use internal::OBJECT_GONE;
pub use internal::OK;
#[doc(hidden)]
pub use internal::ObjectProxy;
#[doc(hidden)]
pub use internal::ObjectProxyCaller;
#[doc(hidden)]
pub use internal::ObjectReleasedParams;
#[doc(hidden)]
pub use internal::ObjectStub;
pub use internal::OpaqueValue;
pub use internal::Optimistic;
pub use internal::OptimisticPtr;
pub use internal::PROXY_DESERIALISATION_ERROR;
#[doc(hidden)]
pub use internal::ParameterDirection;
#[doc(hidden)]
pub use internal::PassThrough;
#[doc(hidden)]
pub use internal::PassThroughKey;
#[doc(hidden)]
pub use internal::PostParams;
#[doc(hidden)]
pub use internal::QueryInterfaceResult;
#[doc(hidden)]
pub use internal::ReferenceRecord;
#[doc(hidden)]
pub use internal::ReleaseParams;
#[doc(hidden)]
pub use internal::RemoteInterfaceView;
#[doc(hidden)]
pub use internal::RemoteObjectBindResult;
#[doc(hidden)]
pub use internal::RemoteObjectResult;
#[doc(hidden)]
pub use internal::RetryBuffer;
#[doc(hidden)]
pub use internal::RpcBase;
#[doc(hidden)]
pub use internal::RpcObject;
pub use internal::STUB_DESERIALISATION_ERROR;
#[doc(hidden)]
pub use internal::SendParams;
#[doc(hidden)]
pub use internal::SendResult;
pub use internal::Service;
pub use internal::ServiceConfig;
pub use internal::ServiceEvent;
#[doc(hidden)]
pub use internal::ServiceProxy;
pub use internal::Shared;
pub use internal::SharedPtr;
pub use internal::StandardResult;
pub use internal::TRANSPORT_ERROR;
#[doc(hidden)]
pub use internal::Transport;
#[doc(hidden)]
pub use internal::TransportDownParams;
#[doc(hidden)]
pub use internal::TransportStatus;
#[doc(hidden)]
pub use internal::TryCastParams;
#[doc(hidden)]
pub use internal::add_ref_options_for_pointer_kind;
#[doc(hidden)]
pub use internal::bind_incoming_optimistic;
#[doc(hidden)]
pub use internal::bind_incoming_shared;
#[doc(hidden)]
pub use internal::bind_outgoing_interface;
pub use internal::bytes_to_string;
pub use internal::get_object_stub;
pub use internal::get_version;
pub use internal::interface_ordinal_to_string;
pub use internal::is_bound_pointer_gone;
pub use internal::is_bound_pointer_null;
pub use internal::is_critical;
pub use internal::is_error;
pub use internal::make_rpc_object_with_adapter;
pub use internal::method_to_string;
pub use internal::null_remote_descriptor;
pub use internal::object_to_string;
pub use internal::release_options_for_pointer_kind;
pub use internal::remote_object_to_string;
pub use internal::zone_address_to_string;
pub use internal::zone_to_string;
pub use rpc_types::AddRefOptions;
pub use rpc_types::AddressType;
pub use rpc_types::BackChannelEntry;
pub use rpc_types::CallerZone;
pub use rpc_types::DefaultValues;
pub use rpc_types::DestinationZone;
pub use rpc_types::Encoding;
pub use rpc_types::InterfaceOrdinal;
pub use rpc_types::Method;
pub use rpc_types::Object;
pub use rpc_types::ReleaseOptions;
pub use rpc_types::RemoteObject;
pub use rpc_types::RequestingZone;
pub use rpc_types::Zone;
pub use rpc_types::ZoneAddress;
pub use rpc_types::ZoneAddressArgs;
