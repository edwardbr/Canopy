//! Rust counterpart of `c++/rpc/include/rpc/internal/bindings_fwd.h`.

use crate::rpc_types::{CallerZone, RemoteObject};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InterfacePointerKind {
    Shared,
    Optimistic,
}

impl InterfacePointerKind {
    pub const fn is_optimistic(self) -> bool {
        matches!(self, Self::Optimistic)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParameterDirection {
    In,
    Out,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InterfaceBindingOrigin {
    Local,
    Remote,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InterfaceBindResult<T> {
    pub error_code: i32,
    pub iface: super::bindings::BoundInterface<T>,
    pub origin: Option<InterfaceBindingOrigin>,
}

impl<T> InterfaceBindResult<T> {
    pub fn new(
        error_code: i32,
        iface: super::bindings::BoundInterface<T>,
        origin: Option<InterfaceBindingOrigin>,
    ) -> Self {
        Self {
            error_code,
            iface,
            origin,
        }
    }

    pub fn null(error_code: i32) -> Self {
        Self::new(error_code, super::bindings::BoundInterface::Null, None)
    }

    pub fn gone(error_code: i32, origin: InterfaceBindingOrigin) -> Self {
        Self::new(
            error_code,
            super::bindings::BoundInterface::Gone,
            Some(origin),
        )
    }

    pub fn local(error_code: i32, iface: T) -> Self {
        Self::new(
            error_code,
            super::bindings::BoundInterface::Value(iface),
            Some(InterfaceBindingOrigin::Local),
        )
    }

    pub fn remote(error_code: i32, iface: T) -> Self {
        Self::new(
            error_code,
            super::bindings::BoundInterface::Value(iface),
            Some(InterfaceBindingOrigin::Remote),
        )
    }

    pub fn is_local(&self) -> bool {
        self.origin == Some(InterfaceBindingOrigin::Local)
    }

    pub fn is_remote(&self) -> bool {
        self.origin == Some(InterfaceBindingOrigin::Remote)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RemoteObjectBindResult<Stub> {
    pub error_code: i32,
    pub stub: Option<Stub>,
    pub descriptor: RemoteObject,
}

impl<Stub> RemoteObjectBindResult<Stub> {
    pub fn new(error_code: i32, stub: Option<Stub>, descriptor: RemoteObject) -> Self {
        Self {
            error_code,
            stub,
            descriptor,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReferenceRecord {
    pub caller_zone_id: CallerZone,
    pub pointer_kind: InterfacePointerKind,
}

impl ReferenceRecord {
    pub fn new(caller_zone_id: CallerZone, pointer_kind: InterfacePointerKind) -> Self {
        Self {
            caller_zone_id,
            pointer_kind,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedInterfaceParamDescriptor {
    pub name: &'static str,
    pub interface_name: &'static str,
    pub pointer_kind: InterfacePointerKind,
    pub direction: ParameterDirection,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedMethodBindingDescriptor {
    pub method_name: &'static str,
    pub method_id: u64,
    pub interface_params: &'static [GeneratedInterfaceParamDescriptor],
}

pub trait GeneratedInterfaceBindingMetadata {
    fn interface_name() -> &'static str;
    fn id_rpc_v2() -> u64;
    fn id_rpc_v3() -> u64;
    fn methods() -> &'static [GeneratedMethodBindingDescriptor];

    fn by_method_id(method_id: u64) -> Option<&'static GeneratedMethodBindingDescriptor> {
        Self::methods()
            .iter()
            .find(|descriptor| descriptor.method_id == method_id)
    }
}

#[cfg(test)]
mod tests {
    use super::{
        GeneratedInterfaceBindingMetadata, GeneratedInterfaceParamDescriptor,
        GeneratedMethodBindingDescriptor,
    };
    use super::{InterfaceBindResult, InterfaceBindingOrigin};
    use super::{InterfacePointerKind, ParameterDirection};
    use crate::internal::bindings::BoundInterface;
    use crate::internal::error_codes;

    #[test]
    fn interface_bind_result_tracks_local_and_remote_origin() {
        let local = InterfaceBindResult::local(error_codes::OK(), 7_u32);
        let remote = InterfaceBindResult::remote(error_codes::OK(), 9_u32);

        assert!(local.is_local());
        assert!(!local.is_remote());
        assert_eq!(local.iface, BoundInterface::Value(7));

        assert!(remote.is_remote());
        assert!(!remote.is_local());
        assert_eq!(remote.iface, BoundInterface::Value(9));
    }

    #[test]
    fn interface_bind_result_preserves_gone_without_looking_null() {
        let gone = InterfaceBindResult::<u32>::gone(
            error_codes::OBJECT_GONE(),
            InterfaceBindingOrigin::Local,
        );

        assert!(gone.is_local());
        assert_eq!(gone.iface, BoundInterface::Gone);
    }

    #[test]
    fn interface_bind_result_null_has_no_origin() {
        let null = InterfaceBindResult::<u32>::null(error_codes::OK());

        assert_eq!(null.origin, None);
        assert_eq!(null.iface, BoundInterface::Null);
    }

    struct ExampleBindingMetadata;

    impl GeneratedInterfaceBindingMetadata for ExampleBindingMetadata {
        fn interface_name() -> &'static str {
            "example::i_demo"
        }

        fn id_rpc_v2() -> u64 {
            2
        }

        fn id_rpc_v3() -> u64 {
            3
        }

        fn methods() -> &'static [GeneratedMethodBindingDescriptor] {
            static PARAMS: [GeneratedInterfaceParamDescriptor; 1] =
                [GeneratedInterfaceParamDescriptor {
                    name: "value",
                    interface_name: "example::i_other",
                    pointer_kind: InterfacePointerKind::Optimistic,
                    direction: ParameterDirection::Out,
                }];
            static METHODS: [GeneratedMethodBindingDescriptor; 1] =
                [GeneratedMethodBindingDescriptor {
                    method_name: "foo",
                    method_id: 7,
                    interface_params: &PARAMS,
                }];
            &METHODS
        }
    }

    #[test]
    fn generated_interface_binding_metadata_default_lookup_works() {
        let method = ExampleBindingMetadata::by_method_id(7).expect("method");
        assert_eq!(ExampleBindingMetadata::interface_name(), "example::i_demo");
        assert_eq!(ExampleBindingMetadata::id_rpc_v2(), 2);
        assert_eq!(ExampleBindingMetadata::id_rpc_v3(), 3);
        assert_eq!(method.method_name, "foo");
        assert_eq!(
            method.interface_params[0].pointer_kind,
            InterfacePointerKind::Optimistic
        );
        assert_eq!(
            method.interface_params[0].direction,
            ParameterDirection::Out
        );
    }
}
