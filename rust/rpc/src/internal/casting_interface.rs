//! Rust counterpart of `c++/rpc/include/rpc/internal/casting_interface.h`.

use std::any::Any;
use std::sync::{Arc, Mutex};

use crate::internal::bindings_fwd::GeneratedMethodBindingDescriptor;
use crate::internal::error_codes;
use crate::internal::marshaller_params::{SendParams, SendResult};
use crate::internal::object_proxy::ObjectProxy;
use crate::internal::stub::ObjectStub;
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

    #[doc(hidden)]
    fn __rpc_remote_object_id(&self) -> Option<RemoteObject> {
        None
    }

    #[doc(hidden)]
    fn __rpc_remote_object_proxy(&self) -> Option<Arc<ObjectProxy>> {
        None
    }

    #[doc(hidden)]
    fn __rpc_local_object_stub(&self) -> Option<Arc<Mutex<ObjectStub>>> {
        None
    }

    #[doc(hidden)]
    fn __rpc_set_stub(&self, _stub: std::sync::Weak<Mutex<ObjectStub>>) {}

    #[doc(hidden)]
    fn __rpc_set_owner_service_ptr(&self, _owner_service: Option<usize>) {}

    #[doc(hidden)]
    fn __rpc_get_local_interface_view(
        self: Arc<Self>,
        _interface_id: InterfaceOrdinal,
    ) -> Option<Arc<dyn Any + Send + Sync>> {
        None
    }

    #[doc(hidden)]
    fn __rpc_owner_service_ptr(&self) -> Option<usize> {
        self.__rpc_local_object_stub().and_then(|stub| {
            stub.lock()
                .expect("object stub mutex poisoned")
                .owner_service_addr()
        })
    }

    #[doc(hidden)]
    fn __rpc_get_method_metadata(
        &self,
        _interface_id: InterfaceOrdinal,
    ) -> &'static [GeneratedMethodBindingDescriptor] {
        &[]
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Weak;

    use super::CastingInterface;
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

    impl CastingInterface for Example {
        fn __rpc_query_interface(&self, interface_id: InterfaceOrdinal) -> bool {
            interface_id == InterfaceOrdinal::new(42)
        }

        fn __rpc_call(&self, params: SendParams) -> crate::SendResult {
            crate::SendResult::new(0, vec![params.method_id.get_val() as u8], vec![])
        }
    }

    #[test]
    fn interface_binding_extends_casting_interface() {
        fn assert_casting<T: CastingInterface>() {}

        assert_casting::<Example>();
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
