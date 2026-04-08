//! Rust-side local transport family.

mod transport;

#[doc(hidden)]
pub use transport::BoundChildZone;
#[doc(hidden)]
pub use transport::ChildTransport;
#[doc(hidden)]
pub use transport::ChildZone;
#[doc(hidden)]
pub use transport::LocalBoundChildZone;
#[doc(hidden)]
pub use transport::LocalChildTransport;
#[doc(hidden)]
pub use transport::LocalChildZone;
#[doc(hidden)]
pub use transport::LocalParentTransport;
#[doc(hidden)]
pub use transport::LocalTransportPair;
#[doc(hidden)]
pub use transport::ParentTransport;
#[doc(hidden)]
pub use transport::TransportPair;

pub fn create_child_zone(
    name: impl Into<String>,
    parent_service: std::sync::Arc<dyn canopy_rpc::ServiceRuntime>,
    child_name: impl Into<String>,
    child_zone_id: canopy_rpc::Zone,
) -> transport::ChildZone {
    transport::ChildZone::create(name, parent_service, child_name, child_zone_id)
}

pub fn create_child_zone_with_exported_object(
    name: impl Into<String>,
    parent_service: std::sync::Arc<dyn canopy_rpc::ServiceRuntime>,
    child_name: impl Into<String>,
    child_zone_id: canopy_rpc::Zone,
    build_exported_object: impl FnOnce(
        std::sync::Arc<canopy_rpc::ChildService>,
        canopy_rpc::Object,
    ) -> std::sync::Arc<dyn canopy_rpc::RpcObject>,
    pointer_kind: canopy_rpc::InterfacePointerKind,
) -> Result<transport::BoundChildZone, i32> {
    let child_zone =
        transport::ChildZone::create(name, parent_service.clone(), child_name, child_zone_id);
    let child_service = child_zone.service().clone();
    let object_id = child_service.generate_new_object_id();
    let exported_object = build_exported_object(child_service, object_id);
    child_zone.bind_root_rpc_object(parent_service, object_id, exported_object, pointer_kind)
}
