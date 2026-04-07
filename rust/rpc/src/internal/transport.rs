//! Rust counterpart of `c++/rpc/include/rpc/internal/transport.h`.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicI64, AtomicU8, Ordering};
use std::sync::{Arc, Mutex, Weak};

use crate::internal::error_codes;
use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
};
use crate::internal::pass_through::{PassThrough, PassThroughKey};
use crate::internal::service::Service;
use crate::rpc_types::{AddRefOptions, CallerZone, DestinationZone, Zone};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TransportStatus {
    Connecting = 0,
    Connected = 1,
    Disconnecting = 2,
    Disconnected = 3,
}

impl TransportStatus {
    fn from_u8(value: u8) -> Self {
        match value {
            1 => Self::Connected,
            2 => Self::Disconnecting,
            3 => Self::Disconnected,
            _ => Self::Connecting,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
struct RemoteServiceCount {
    outbound_proxy_count: u64,
    inbound_stub_count: u64,
}

pub struct Transport {
    name: String,
    zone_id: Zone,
    adjacent_zone_id: Zone,
    service: Mutex<Weak<Service>>,
    outbound: Arc<dyn IMarshaller + Send + Sync>,
    pass_throughs: Mutex<BTreeMap<PassThroughKey, Weak<PassThrough>>>,
    zone_counts: Mutex<BTreeMap<Zone, RemoteServiceCount>>,
    destination_count: AtomicI64,
    status: AtomicU8,
}

impl std::fmt::Debug for Transport {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Transport")
            .field("name", &self.name)
            .field("zone_id", &self.zone_id)
            .field("adjacent_zone_id", &self.adjacent_zone_id)
            .field("destination_count", &self.destination_count())
            .field("status", &self.status())
            .finish_non_exhaustive()
    }
}

impl Transport {
    pub fn new(
        name: impl Into<String>,
        zone_id: Zone,
        adjacent_zone_id: Zone,
        service: Weak<Service>,
        outbound: Arc<dyn IMarshaller + Send + Sync>,
    ) -> Arc<Self> {
        Arc::new(Self {
            name: name.into(),
            zone_id,
            adjacent_zone_id,
            service: Mutex::new(service),
            outbound,
            pass_throughs: Mutex::new(BTreeMap::new()),
            zone_counts: Mutex::new(BTreeMap::new()),
            destination_count: AtomicI64::new(0),
            status: AtomicU8::new(TransportStatus::Connecting as u8),
        })
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn zone_id(&self) -> Zone {
        self.zone_id.clone()
    }

    pub fn adjacent_zone_id(&self) -> Zone {
        self.adjacent_zone_id.clone()
    }

    pub fn service(&self) -> Option<Arc<Service>> {
        self.service
            .lock()
            .expect("transport service mutex poisoned")
            .upgrade()
    }

    pub fn set_service(&self, service: Weak<Service>) {
        *self
            .service
            .lock()
            .expect("transport service mutex poisoned") = service;
    }

    pub fn status(&self) -> TransportStatus {
        TransportStatus::from_u8(self.status.load(Ordering::Acquire))
    }

    pub fn set_status(&self, status: TransportStatus) {
        self.status.store(status as u8, Ordering::Release);
    }

    pub fn destination_count(&self) -> i64 {
        self.destination_count.load(Ordering::Acquire)
    }

    pub fn increment_outbound_proxy_count(&self, dest: DestinationZone) {
        self.update_zone_count(dest, |count| {
            count.outbound_proxy_count += 1;
        });
    }

    pub fn decrement_outbound_proxy_count(&self, dest: DestinationZone) -> bool {
        self.update_zone_count(dest, |count| {
            count.outbound_proxy_count = count.outbound_proxy_count.saturating_sub(1);
        })
    }

    pub fn increment_inbound_stub_count(&self, caller: CallerZone) {
        self.update_zone_count(caller, |count| {
            count.inbound_stub_count += 1;
        });
    }

    pub fn decrement_inbound_stub_count(&self, caller: CallerZone) -> bool {
        self.decrement_inbound_stub_count_by(caller, 1)
    }

    pub fn decrement_inbound_stub_count_by(&self, caller: CallerZone, decrement: u64) -> bool {
        self.update_zone_count(caller, |count| {
            count.inbound_stub_count = count.inbound_stub_count.saturating_sub(decrement);
        })
    }

    fn update_zone_count(&self, zone: Zone, update: impl FnOnce(&mut RemoteServiceCount)) -> bool {
        let mut counts = self
            .zone_counts
            .lock()
            .expect("transport zone_counts mutex poisoned");
        let previous_total = counts.values().filter(|count| count.total() > 0).count();
        let entry = counts.entry(zone.clone()).or_default();
        update(entry);
        if entry.total() == 0 {
            counts.remove(&zone);
        }
        let new_total = counts.values().filter(|count| count.total() > 0).count();
        self.destination_count
            .store(new_total as i64, Ordering::Release);
        previous_total != 0 && new_total == 0
    }

    pub fn add_passthrough(&self, pass_through: &Arc<PassThrough>) {
        self.pass_throughs
            .lock()
            .expect("transport pass_throughs mutex poisoned")
            .insert(pass_through.key(), Arc::downgrade(pass_through));
    }

    pub fn get_passthrough(
        &self,
        zone1: DestinationZone,
        zone2: DestinationZone,
    ) -> Option<Arc<PassThrough>> {
        let key = PassThroughKey::new(zone1, zone2);
        let mut pass_throughs = self
            .pass_throughs
            .lock()
            .expect("transport pass_throughs mutex poisoned");
        match pass_throughs.get(&key).and_then(Weak::upgrade) {
            Some(pass_through) => Some(pass_through),
            None => {
                pass_throughs.remove(&key);
                None
            }
        }
    }

    pub fn pass_through_entry_count_for_test(&self) -> usize {
        self.pass_throughs
            .lock()
            .expect("transport pass_throughs mutex poisoned")
            .len()
    }

    pub fn create_pass_through(
        forward: Arc<Transport>,
        reverse: Arc<Transport>,
        service: Weak<Service>,
        forward_dest: DestinationZone,
        reverse_dest: DestinationZone,
    ) -> Option<Arc<PassThrough>> {
        if forward_dest == reverse_dest || Arc::ptr_eq(&forward, &reverse) {
            return None;
        }

        if let Some(existing) = forward.get_passthrough(forward_dest.clone(), reverse_dest.clone())
        {
            return Some(existing);
        }

        let pass_through = Arc::new(PassThrough::new(
            forward.clone(),
            reverse.clone(),
            service,
            forward_dest,
            reverse_dest,
        ));
        forward.add_passthrough(&pass_through);
        reverse.add_passthrough(&pass_through);
        Some(pass_through)
    }

    pub fn inbound_add_ref(self: &Arc<Self>, params: AddRefParams) -> StandardResult {
        let Some(service) = self.service() else {
            return StandardResult::new(error_codes::TRANSPORT_ERROR(), vec![]);
        };

        let build_caller_channel =
            !(params.build_out_param_channel & AddRefOptions::BUILD_CALLER_ROUTE).is_empty();
        let build_dest_channel =
            !(params.build_out_param_channel & AddRefOptions::BUILD_DESTINATION_ROUTE).is_empty()
                || params.build_out_param_channel == AddRefOptions::NORMAL
                || params.build_out_param_channel == AddRefOptions::OPTIMISTIC;

        if !params
            .remote_object_id
            .get_address()
            .same_zone(service.zone_id().get_address())
            && params.caller_zone_id != service.zone_id()
        {
            let remote_zone = params.remote_object_id.as_zone();
            let mut dest_transport = service.get_transport(&remote_zone);
            if params
                .remote_object_id
                .get_address()
                .same_zone(params.caller_zone_id.get_address())
            {
                return dest_transport
                    .map(|transport| transport.add_ref(params.clone()))
                    .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]));
            }

            if dest_transport.is_none() {
                if build_dest_channel {
                    dest_transport = self
                        .get_transport_from_passthroughs(&remote_zone)
                        .or_else(|| service.get_transport(&params.requesting_zone_id))
                        .or_else(|| {
                            if remote_zone != params.requesting_zone_id {
                                self.get_transport_from_passthroughs(&params.requesting_zone_id)
                            } else {
                                None
                            }
                        });
                    let Some(transport) = dest_transport.clone() else {
                        return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
                    };
                    service.add_transport(remote_zone.clone(), transport);
                } else {
                    dest_transport = Some(self.clone());
                }
            }

            let Some(dest_transport) = dest_transport else {
                return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
            };

            if let Some(pass_through) =
                dest_transport.get_passthrough(params.caller_zone_id.clone(), remote_zone.clone())
            {
                return pass_through.add_ref(params);
            }

            let mut caller_transport = service.get_transport(&params.caller_zone_id);
            if caller_transport.is_none() {
                if !build_dest_channel && build_caller_channel {
                    caller_transport = self
                        .get_transport_from_passthroughs(&params.caller_zone_id)
                        .or_else(|| service.get_transport(&params.requesting_zone_id))
                        .or_else(|| {
                            if params.caller_zone_id != params.requesting_zone_id {
                                self.get_transport_from_passthroughs(&params.requesting_zone_id)
                            } else {
                                None
                            }
                        });
                    let Some(transport) = caller_transport.clone() else {
                        return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
                    };
                    service.add_transport(params.caller_zone_id.clone(), transport);
                } else {
                    caller_transport = Some(self.clone());
                }
            }

            let Some(caller_transport) = caller_transport else {
                return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]);
            };

            if Arc::ptr_eq(&dest_transport, &caller_transport) {
                return dest_transport.add_ref(params);
            }

            let pass_through = Self::create_pass_through(
                dest_transport,
                caller_transport,
                Arc::downgrade(&service),
                remote_zone,
                params.caller_zone_id.clone(),
            );
            return pass_through
                .map(|pass_through| pass_through.add_ref(params))
                .unwrap_or_else(|| StandardResult::new(error_codes::TRANSPORT_ERROR(), vec![]));
        }

        service.add_ref(params)
    }

    pub fn inbound_release(self: &Arc<Self>, params: ReleaseParams) -> StandardResult {
        let Some(service) = self.service() else {
            return StandardResult::new(error_codes::TRANSPORT_ERROR(), vec![]);
        };

        if params
            .remote_object_id
            .get_address()
            .same_zone(service.zone_id().get_address())
        {
            return service.release(params);
        }

        let remote_zone = params.remote_object_id.as_zone();
        self.get_passthrough(remote_zone, params.caller_zone_id.clone())
            .map(|pass_through| pass_through.release(params))
            .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]))
    }

    pub fn get_transport_from_passthroughs(&self, destination: &Zone) -> Option<Arc<Transport>> {
        let pass_throughs: Vec<Arc<PassThrough>> = self
            .pass_throughs
            .lock()
            .expect("transport pass_throughs mutex poisoned")
            .values()
            .filter_map(Weak::upgrade)
            .collect();
        for pass_through in pass_throughs {
            if let Some(transport) = pass_through.transport_for_destination(destination) {
                return Some(transport);
            }
        }
        None
    }
}

impl RemoteServiceCount {
    fn total(self) -> u64 {
        self.outbound_proxy_count + self.inbound_stub_count
    }
}

impl IMarshaller for Transport {
    fn send(&self, params: SendParams) -> SendResult {
        self.outbound.send(params)
    }

    fn post(&self, params: PostParams) {
        self.outbound.post(params);
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.outbound.try_cast(params)
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.outbound.add_ref(params)
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        self.outbound.release(params)
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        self.outbound.object_released(params);
    }

    fn transport_down(&self, params: TransportDownParams) {
        self.outbound.transport_down(params);
    }

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        self.outbound.get_new_zone_id(params)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use super::{Transport, TransportStatus};
    use crate::InterfacePointerKind;
    use crate::internal::marshaller::IMarshaller;
    use crate::internal::marshaller_params::{
        AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
        ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
    };
    use crate::internal::stub::ObjectStub;
    use crate::internal::{Service, error_codes};
    use crate::rpc_types::{
        AddRefOptions, AddressType, CallerZone, DefaultValues, Object, ReleaseOptions,
        RemoteObject, Zone, ZoneAddress, ZoneAddressArgs,
    };

    #[derive(Default)]
    struct RecordingMarshaller {
        last_add_ref: Mutex<Option<AddRefParams>>,
        last_release: Mutex<Option<ReleaseParams>>,
    }

    impl IMarshaller for RecordingMarshaller {
        fn send(&self, _params: SendParams) -> SendResult {
            SendResult::new(error_codes::OK(), vec![], vec![])
        }

        fn post(&self, _params: PostParams) {}

        fn try_cast(&self, _params: TryCastParams) -> StandardResult {
            StandardResult::new(error_codes::OK(), vec![])
        }

        fn add_ref(&self, params: AddRefParams) -> StandardResult {
            *self
                .last_add_ref
                .lock()
                .expect("last_add_ref mutex poisoned") = Some(params);
            StandardResult::new(error_codes::OK(), vec![])
        }

        fn release(&self, params: ReleaseParams) -> StandardResult {
            *self
                .last_release
                .lock()
                .expect("last_release mutex poisoned") = Some(params);
            StandardResult::new(error_codes::OK(), vec![])
        }

        fn object_released(&self, _params: ObjectReleasedParams) {}

        fn transport_down(&self, _params: TransportDownParams) {}

        fn get_new_zone_id(&self, _params: GetNewZoneIdParams) -> NewZoneIdResult {
            NewZoneIdResult::default()
        }
    }

    fn zone(subnet: u64) -> Zone {
        Zone::from(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Local,
                0,
                vec![],
                DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
                subnet,
                DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
                0,
                vec![],
            ))
            .expect("zone address"),
        )
    }

    fn remote_object(subnet: u64, object: u64) -> RemoteObject {
        RemoteObject::from(
            zone(subnet)
                .with_object(Object::new(object))
                .expect("remote object"),
        )
    }

    #[test]
    fn service_routes_remote_add_ref_through_requesting_zone_transport() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let transport = Transport::new(
            "requesting",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            marshaller.clone(),
        );
        transport.set_status(TransportStatus::Connected);
        service.add_transport(zone(2), transport.clone());

        let result = service.add_ref(AddRefParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(3, 77),
            caller_zone_id: CallerZone::from(zone(1)),
            requesting_zone_id: zone(2),
            build_out_param_channel: AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        assert!(service.get_transport(&zone(3)).is_some());
        let captured = marshaller
            .last_add_ref
            .lock()
            .expect("last_add_ref mutex poisoned")
            .clone()
            .expect("add_ref should be forwarded");
        assert_eq!(captured.remote_object_id, remote_object(3, 77));
        assert_eq!(captured.requesting_zone_id, zone(2));
    }

    #[test]
    fn service_routes_remote_release_through_registered_transport() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let transport = Transport::new(
            "requesting",
            zone(1),
            zone(5),
            Arc::downgrade(&service),
            marshaller.clone(),
        );
        transport.set_status(TransportStatus::Connected);
        service.add_transport(zone(7), transport.clone());

        let result = service.release(ReleaseParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(7, 77),
            caller_zone_id: CallerZone::from(zone(1)),
            options: ReleaseOptions::OPTIMISTIC,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        let captured = marshaller
            .last_release
            .lock()
            .expect("last_release mutex poisoned")
            .clone()
            .expect("release should be forwarded through registered transport");
        assert_eq!(captured.remote_object_id, remote_object(7, 77));
        assert_eq!(captured.caller_zone_id, CallerZone::from(zone(1)));
        assert_eq!(captured.options, ReleaseOptions::OPTIMISTIC);
    }

    #[test]
    fn inbound_add_ref_uses_requesting_zone_for_unknown_y_topology_destination() {
        let service = Arc::new(Service::new("zone1", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let zone5_transport = Transport::new(
            "zone1-to-zone5",
            zone(1),
            zone(5),
            Arc::downgrade(&service),
            marshaller.clone(),
        );
        zone5_transport.set_status(TransportStatus::Connected);
        service.add_transport(zone(5), zone5_transport.clone());

        let result = zone5_transport.inbound_add_ref(AddRefParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(7, 77),
            caller_zone_id: CallerZone::from(zone(1)),
            requesting_zone_id: zone(5),
            build_out_param_channel: AddRefOptions::BUILD_DESTINATION_ROUTE
                | AddRefOptions::BUILD_CALLER_ROUTE
                | AddRefOptions::NORMAL,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        assert!(service.get_transport(&zone(7)).is_some());
        let captured = marshaller
            .last_add_ref
            .lock()
            .expect("last_add_ref mutex poisoned")
            .clone()
            .expect("add_ref should be forwarded through requesting-zone transport");
        assert_eq!(captured.remote_object_id, remote_object(7, 77));
        assert_eq!(captured.caller_zone_id, CallerZone::from(zone(1)));
        assert_eq!(captured.requesting_zone_id, zone(5));
        assert_eq!(
            captured.build_out_param_channel,
            AddRefOptions::BUILD_DESTINATION_ROUTE
        );
    }

    #[test]
    fn service_add_ref_caller_route_only_seeds_route_without_forwarding() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let transport = Transport::new(
            "requesting",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            marshaller.clone(),
        );
        service.add_transport(zone(2), transport.clone());

        let result = service.add_ref(AddRefParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(3, 88),
            caller_zone_id: CallerZone::from(zone(1)),
            requesting_zone_id: zone(2),
            build_out_param_channel: AddRefOptions::BUILD_CALLER_ROUTE,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        assert!(service.get_transport(&zone(3)).is_some());
        assert!(
            marshaller
                .last_add_ref
                .lock()
                .expect("last_add_ref mutex poisoned")
                .is_none()
        );
    }

    #[test]
    fn service_add_ref_same_zone_caller_route_only_requires_route_not_stub() {
        let service = Arc::new(Service::new("svc", zone(1)));

        let result = service.add_ref(AddRefParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(1, 99),
            caller_zone_id: CallerZone::from(zone(1)),
            requesting_zone_id: zone(1),
            build_out_param_channel: AddRefOptions::BUILD_CALLER_ROUTE,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::ZONE_NOT_FOUND());
    }

    #[test]
    fn service_add_ref_accepts_dummy_local_destination_object() {
        let service = Arc::new(Service::new("svc", zone(1)));

        let result = service.add_ref(AddRefParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: RemoteObject::from(
                zone(1)
                    .with_object(crate::DUMMY_OBJECT_ID)
                    .expect("dummy remote object"),
            ),
            caller_zone_id: CallerZone::from(zone(2)),
            requesting_zone_id: zone(2),
            build_out_param_channel: AddRefOptions::NORMAL,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
    }

    #[test]
    fn transport_tracks_destination_counts_by_zone() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let transport = Transport::new(
            "direct",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            marshaller,
        );

        transport.increment_outbound_proxy_count(zone(2));
        transport.increment_inbound_stub_count(zone(2));
        assert_eq!(transport.destination_count(), 1);

        assert!(!transport.decrement_outbound_proxy_count(zone(2)));
        assert_eq!(transport.destination_count(), 1);

        assert!(transport.decrement_inbound_stub_count(zone(2)));
        assert_eq!(transport.destination_count(), 0);
    }

    #[test]
    fn transport_creates_shared_pass_through_for_zone_pair() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let forward = Transport::new(
            "forward",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            Arc::new(RecordingMarshaller::default()),
        );
        let reverse = Transport::new(
            "reverse",
            zone(1),
            zone(3),
            Arc::downgrade(&service),
            Arc::new(RecordingMarshaller::default()),
        );

        let first = Transport::create_pass_through(
            forward.clone(),
            reverse.clone(),
            Arc::downgrade(&service),
            zone(2),
            zone(3),
        )
        .expect("pass through should be created");
        let second = Transport::create_pass_through(
            forward.clone(),
            reverse.clone(),
            Arc::downgrade(&service),
            zone(3),
            zone(2),
        )
        .expect("existing pass through should be reused");

        assert!(Arc::ptr_eq(&first, &second));
        assert!(Arc::ptr_eq(
            &first
                .transport_for_destination(&zone(2))
                .expect("forward route"),
            &forward
        ));
        assert!(Arc::ptr_eq(
            &second
                .transport_for_destination(&zone(3))
                .expect("reverse route"),
            &reverse
        ));
    }

    #[test]
    fn transport_prunes_dead_passthrough_entries_on_lookup() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let forward = Transport::new(
            "forward",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            Arc::new(RecordingMarshaller::default()),
        );
        let reverse = Transport::new(
            "reverse",
            zone(1),
            zone(3),
            Arc::downgrade(&service),
            Arc::new(RecordingMarshaller::default()),
        );

        let pass_through = Transport::create_pass_through(
            forward.clone(),
            reverse.clone(),
            Arc::downgrade(&service),
            zone(2),
            zone(3),
        )
        .expect("pass-through should be created");
        assert_eq!(forward.pass_through_entry_count_for_test(), 1);
        assert_eq!(reverse.pass_through_entry_count_for_test(), 1);

        drop(pass_through);

        assert!(forward.get_passthrough(zone(2), zone(3)).is_none());
        assert!(reverse.get_passthrough(zone(2), zone(3)).is_none());
        assert_eq!(forward.pass_through_entry_count_for_test(), 0);
        assert_eq!(reverse.pass_through_entry_count_for_test(), 0);
    }

    #[test]
    fn inbound_release_for_local_destination_releases_local_stub() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let transport = Transport::new(
            "inbound",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            marshaller,
        );
        let stub = Arc::new(Mutex::new(ObjectStub::new(Object::new(77))));
        service.register_stub(&stub);
        stub.lock()
            .expect("stub mutex poisoned")
            .add_ref(InterfacePointerKind::Shared, CallerZone::from(zone(2)));

        let result = transport.inbound_release(ReleaseParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(1, 77),
            caller_zone_id: CallerZone::from(zone(2)),
            options: ReleaseOptions::NORMAL,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        assert!(service.get_object(Object::new(77)).is_none());
    }

    #[test]
    fn inbound_release_for_remote_destination_uses_existing_pass_through() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let forward_marshaller = Arc::new(RecordingMarshaller::default());
        let reverse_marshaller = Arc::new(RecordingMarshaller::default());
        let forward = Transport::new(
            "forward",
            zone(1),
            zone(2),
            Arc::downgrade(&service),
            forward_marshaller.clone(),
        );
        let reverse = Transport::new(
            "reverse",
            zone(1),
            zone(3),
            Arc::downgrade(&service),
            reverse_marshaller.clone(),
        );
        let _pass_through = Transport::create_pass_through(
            forward.clone(),
            reverse.clone(),
            Arc::downgrade(&service),
            zone(2),
            zone(3),
        )
        .expect("pass-through should be created");

        let result = forward.inbound_release(ReleaseParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(2, 88),
            caller_zone_id: CallerZone::from(zone(3)),
            options: ReleaseOptions::OPTIMISTIC,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::OK());
        let captured = forward_marshaller
            .last_release
            .lock()
            .expect("last_release mutex poisoned")
            .clone()
            .expect("release should be forwarded to destination transport");
        assert_eq!(captured.remote_object_id, remote_object(2, 88));
        assert_eq!(captured.caller_zone_id, CallerZone::from(zone(3)));
        assert_eq!(captured.options, ReleaseOptions::OPTIMISTIC);
        assert!(
            reverse_marshaller
                .last_release
                .lock()
                .expect("last_release mutex poisoned")
                .is_none()
        );
    }

    #[test]
    fn inbound_release_for_remote_destination_without_pass_through_fails() {
        let service = Arc::new(Service::new("svc", zone(1)));
        let marshaller = Arc::new(RecordingMarshaller::default());
        let transport = Transport::new(
            "inbound",
            zone(1),
            zone(5),
            Arc::downgrade(&service),
            marshaller.clone(),
        );

        let result = transport.inbound_release(ReleaseParams {
            protocol_version: crate::version::get_version(),
            remote_object_id: remote_object(7, 99),
            caller_zone_id: CallerZone::from(zone(1)),
            options: ReleaseOptions::NORMAL,
            in_back_channel: vec![],
        });

        assert_eq!(result.error_code, error_codes::ZONE_NOT_FOUND());
        assert!(
            marshaller
                .last_release
                .lock()
                .expect("last_release mutex poisoned")
                .is_none()
        );
    }
}
