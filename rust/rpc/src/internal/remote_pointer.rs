//! Rust counterpart of `c++/rpc/include/rpc/internal/remote_pointer.h`.
//!
//! The important semantic split to preserve from C++ is:
//! - `shared_ptr` and `optimistic_ptr` both work for local and remote objects
//! - the difference is ownership/lifetime behavior, not locality
//! - local optimistic bindings are represented via a local proxy over a weak
//!   local target instead of a remote-object descriptor

use std::sync::{Arc, Weak};

use crate::internal::service_proxy::GeneratedRpcCaller;

/// Minimal handwritten equivalent of C++ `rpc::local_proxy<T>`.
///
/// This does not yet forward generated interface methods. Its current role is
/// to preserve the weak/local optimistic binding shape so the Rust runtime and
/// generator can distinguish:
/// - optimistic binding to a local object via weak semantics
/// - optimistic binding to a remote object via remote proxy semantics
#[derive(Debug)]
pub struct LocalProxy<T> {
    weak: Weak<T>,
    remote: Option<Arc<T>>,
    was_bound: bool,
}

impl<T> LocalProxy<T> {
    pub fn new(weak: Weak<T>) -> Self {
        Self {
            weak,
            remote: None,
            was_bound: true,
        }
    }

    pub fn null() -> Self {
        Self {
            weak: Weak::new(),
            remote: None,
            was_bound: false,
        }
    }

    pub fn from_shared(shared: &Arc<T>) -> Self {
        Self::new(Arc::downgrade(shared))
    }

    pub fn from_remote(remote: Arc<T>) -> Self {
        Self {
            weak: Weak::new(),
            remote: Some(remote),
            was_bound: true,
        }
    }

    pub fn get_weak(&self) -> Weak<T> {
        self.weak.clone()
    }

    pub fn upgrade(&self) -> Option<Arc<T>> {
        self.remote.clone().or_else(|| self.weak.upgrade())
    }

    pub fn expired(&self) -> bool {
        self.upgrade().is_none()
    }

    pub fn is_null(&self) -> bool {
        !self.was_bound
    }
}

impl<T> Clone for LocalProxy<T> {
    fn clone(&self) -> Self {
        Self {
            weak: self.weak.clone(),
            remote: self.remote.clone(),
            was_bound: self.was_bound,
        }
    }
}

pub trait CreateLocalProxy: Sized {
    fn create_local_proxy(weak: Weak<Self>) -> LocalProxy<Self> {
        LocalProxy::new(weak)
    }
}

pub trait CreateRemoteProxy: Sized {
    fn create_remote_proxy(caller: Arc<dyn GeneratedRpcCaller>) -> Self;
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::CreateLocalProxy;
    use super::LocalProxy;

    #[derive(Debug)]
    struct Example;

    impl CreateLocalProxy for Example {}

    #[test]
    fn local_proxy_upgrades_while_target_is_alive() {
        let shared = Arc::new(Example);
        let proxy = LocalProxy::from_shared(&shared);

        assert!(!proxy.is_null());
        assert!(!proxy.expired());
        assert!(proxy.upgrade().is_some());
    }

    #[test]
    fn local_proxy_expires_without_becoming_null() {
        let shared = Arc::new(Example);
        let proxy = Example::create_local_proxy(Arc::downgrade(&shared));
        drop(shared);

        assert!(!proxy.is_null());
        assert!(proxy.expired());
        assert!(proxy.upgrade().is_none());
    }

    #[test]
    fn default_local_proxy_is_null() {
        let proxy = LocalProxy::<Example>::null();

        assert!(proxy.is_null());
        assert!(proxy.expired());
        assert!(proxy.upgrade().is_none());
    }

    #[test]
    fn remote_local_proxy_keeps_remote_proxy_view_alive() {
        let proxy = LocalProxy::from_remote(Arc::new(Example));

        assert!(!proxy.is_null());
        assert!(!proxy.expired());
        assert!(proxy.upgrade().is_some());
    }
}
