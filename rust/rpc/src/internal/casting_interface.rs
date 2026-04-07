//! Rust counterpart of `c++/rpc/include/rpc/internal/casting_interface.h`.

use std::any::Any;
use std::sync::Arc;

use crate::internal::bindings_fwd::GeneratedMethodBindingDescriptor;
use crate::internal::error_codes;
use crate::internal::marshaller_params::{SendParams, SendResult};
use crate::internal::object_proxy::ObjectProxy;
use crate::internal::remote_pointer::CreateLocalProxy;
use crate::internal::service_proxy::GeneratedRpcCaller;
use crate::rpc_types::{InterfaceOrdinal, RemoteObject};

#[doc(hidden)]
pub trait CastingInterface: Any + Send + Sync + 'static {
    #[doc(hidden)]
    fn __rpc_query_interface(&self, _interface_id: InterfaceOrdinal) -> bool {
        false
    }

    #[doc(hidden)]
    fn __rpc_call(&self, _params: SendParams) -> SendResult {
        SendResult::new(error_codes::INVALID_INTERFACE_ID(), vec![], vec![])
    }
}

#[doc(hidden)]
pub trait GeneratedRustInterface: CastingInterface + CreateLocalProxy {
    #[doc(hidden)]
    fn interface_name() -> &'static str
    where
        Self: Sized;

    #[doc(hidden)]
    fn get_id(rpc_version: u64) -> u64
    where
        Self: Sized;

    #[doc(hidden)]
    fn binding_metadata() -> &'static [GeneratedMethodBindingDescriptor]
    where
        Self: Sized;

    #[doc(hidden)]
    fn create_remote_proxy(caller: Arc<dyn GeneratedRpcCaller>) -> Self
    where
        Self: Sized,
    {
        let _ = caller;
        panic!("remote proxy construction is only implemented by generated proxy skeletons")
    }

    #[doc(hidden)]
    fn remote_object_id(&self) -> Option<RemoteObject> {
        None
    }

    #[doc(hidden)]
    fn remote_object_proxy(&self) -> Option<Arc<ObjectProxy>> {
        None
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Weak;

    use super::{CastingInterface, GeneratedRustInterface};
    use crate::internal::marshaller_params::SendParams;
    use crate::internal::remote_pointer::LocalProxy;
    use crate::rpc_types::{InterfaceOrdinal, Method};

    #[derive(Debug)]
    struct Example;

    impl crate::internal::remote_pointer::CreateLocalProxy for Example {
        fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
            LocalProxy::new(weak)
        }
    }

    impl GeneratedRustInterface for Example {
        fn interface_name() -> &'static str {
            "example::i_demo"
        }

        fn get_id(_rpc_version: u64) -> u64 {
            42
        }

        fn binding_metadata() -> &'static [crate::GeneratedMethodBindingDescriptor] {
            &[]
        }
    }

    impl CastingInterface for Example {
        fn __rpc_query_interface(&self, interface_id: InterfaceOrdinal) -> bool {
            interface_id == InterfaceOrdinal::new(42)
        }

        fn __rpc_call(&self, params: SendParams) -> crate::SendResult {
            crate::SendResult::new(0, vec![params.method_id.get_val() as u8], vec![])
        }
    }

    #[test]
    fn generated_rust_interface_extends_casting_interface() {
        fn assert_casting<T: CastingInterface>() {}

        assert_casting::<Example>();
        assert_eq!(Example::interface_name(), "example::i_demo");
        assert_eq!(Example::get_id(3), 42);
        assert!(Example::binding_metadata().is_empty());
        let example = Example;
        assert!(example.__rpc_query_interface(InterfaceOrdinal::new(42)));
        assert_eq!(
            example
                .__rpc_call(SendParams {
                    method_id: Method::new(7),
                    ..Default::default()
                })
                .out_buf,
            vec![7]
        );
    }
}
