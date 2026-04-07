#[test]
fn generated_remote_binding_sends_y_topology_add_ref_to_cxx_transport() {
    let _guard = CXX_DLL_TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let Some(runtime) = create_cxx_probe_runtime(5, 7, false) else {
        return;
    };

    let mut created_shared: canopy_rpc::Shared<Arc<i_peer::ProxySkeleton>> =
        canopy_rpc::Shared::null();
    assert_eq!(
        runtime.proxy.create_shared_peer(&mut created_shared),
        canopy_rpc::OK()
    );
    let canopy_rpc::BoundInterface::Value(created_peer) = created_shared.into_inner() else {
        panic!("expected C++ created peer");
    };
    let descriptor = canopy_rpc::GeneratedRustInterface::remote_object_id(created_peer.as_ref())
        .expect("generated C++ peer proxy should carry a remote descriptor");

    *runtime
        .last_add_ref
        .lock()
        .expect("last_add_ref mutex poisoned") = None;

    let bind_result = i_math::interface_binding::accept_shared_peer::bind_peer_outgoing_remote(
        canopy_rpc::CallerZone::from(sample_zone(1)),
        &canopy_rpc::Shared::from_value(created_peer.clone()),
    );

    assert_eq!(bind_result.error_code, canopy_rpc::OK());
    assert_eq!(bind_result.descriptor, descriptor);

    let add_ref = runtime
        .last_add_ref
        .lock()
        .expect("last_add_ref mutex poisoned")
        .clone()
        .expect("generated remote binding should send add_ref over the C++ DLL transport");
    assert_eq!(add_ref.remote_object_id, descriptor);
    assert_eq!(
        add_ref.caller_zone_id,
        canopy_rpc::CallerZone::from(sample_zone(1))
    );
    assert_eq!(add_ref.requesting_zone_id, sample_zone(5));
    assert_eq!(
        add_ref.build_out_param_channel,
        canopy_rpc::AddRefOptions::BUILD_DESTINATION_ROUTE
            | canopy_rpc::AddRefOptions::BUILD_CALLER_ROUTE
            | canopy_rpc::AddRefOptions::NORMAL
    );

    *runtime
        .last_release
        .lock()
        .expect("last_release mutex poisoned") = None;
    let object_proxy =
        canopy_rpc::GeneratedRustInterface::remote_object_proxy(created_peer.as_ref())
            .expect("generated C++ peer proxy should carry an object proxy");
    let release_result = object_proxy.release_remote_for_caller(
        canopy_rpc::CallerZone::from(sample_zone(1)),
        canopy_rpc::ReleaseOptions::NORMAL,
    );
    assert_eq!(release_result.error_code, canopy_rpc::OK());

    let release = runtime
        .last_release
        .lock()
        .expect("last_release mutex poisoned")
        .clone()
        .expect("generated remote binding release should cross the C++ DLL transport");
    assert_eq!(release.remote_object_id, descriptor);
    assert_eq!(
        release.caller_zone_id,
        canopy_rpc::CallerZone::from(sample_zone(1))
    );
    assert_eq!(release.options, canopy_rpc::ReleaseOptions::NORMAL);
}

#[test]
fn generated_optimistic_remote_binding_sends_y_topology_add_ref_to_cxx_transport() {
    let _guard = CXX_DLL_TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let Some(runtime) = create_cxx_probe_runtime(5, 7, false) else {
        return;
    };

    let mut created_shared: canopy_rpc::Shared<Arc<i_peer::ProxySkeleton>> =
        canopy_rpc::Shared::null();
    assert_eq!(
        runtime.proxy.create_shared_peer(&mut created_shared),
        canopy_rpc::OK()
    );
    let canopy_rpc::BoundInterface::Value(created_peer) = created_shared.into_inner() else {
        panic!("expected C++ created peer");
    };
    let descriptor = canopy_rpc::GeneratedRustInterface::remote_object_id(created_peer.as_ref())
        .expect("generated C++ peer proxy should carry a remote descriptor");
    let created_peer = canopy_rpc::LocalProxy::from_shared(&created_peer);

    *runtime
        .last_add_ref
        .lock()
        .expect("last_add_ref mutex poisoned") = None;

    let bind_result = i_math::interface_binding::accept_optimistic_peer::bind_peer_outgoing_remote(
        canopy_rpc::CallerZone::from(sample_zone(1)),
        &canopy_rpc::Optimistic::from_value(created_peer.clone()),
    );

    assert_eq!(bind_result.error_code, canopy_rpc::OK());
    assert_eq!(bind_result.descriptor, descriptor);

    let add_ref = runtime
        .last_add_ref
        .lock()
        .expect("last_add_ref mutex poisoned")
        .clone()
        .expect("generated remote binding should send add_ref over the C++ DLL transport");
    assert_eq!(add_ref.remote_object_id, descriptor);
    assert_eq!(
        add_ref.caller_zone_id,
        canopy_rpc::CallerZone::from(sample_zone(1))
    );
    assert_eq!(add_ref.requesting_zone_id, sample_zone(5));
    assert_eq!(
        add_ref.build_out_param_channel,
        canopy_rpc::AddRefOptions::BUILD_DESTINATION_ROUTE
            | canopy_rpc::AddRefOptions::BUILD_CALLER_ROUTE
            | canopy_rpc::AddRefOptions::OPTIMISTIC
    );

    *runtime
        .last_release
        .lock()
        .expect("last_release mutex poisoned") = None;
    let created_peer = created_peer
        .upgrade()
        .expect("remote optimistic proxy should upgrade");
    let object_proxy =
        canopy_rpc::GeneratedRustInterface::remote_object_proxy(created_peer.as_ref())
            .expect("generated C++ peer proxy should carry an object proxy");
    let release_result = object_proxy.release_remote_for_caller(
        canopy_rpc::CallerZone::from(sample_zone(1)),
        canopy_rpc::ReleaseOptions::OPTIMISTIC,
    );
    assert_eq!(release_result.error_code, canopy_rpc::OK());

    let release = runtime
        .last_release
        .lock()
        .expect("last_release mutex poisoned")
        .clone()
        .expect("generated remote binding release should cross the C++ DLL transport");
    assert_eq!(release.remote_object_id, descriptor);
    assert_eq!(
        release.caller_zone_id,
        canopy_rpc::CallerZone::from(sample_zone(1))
    );
    assert_eq!(release.options, canopy_rpc::ReleaseOptions::OPTIMISTIC);
}
