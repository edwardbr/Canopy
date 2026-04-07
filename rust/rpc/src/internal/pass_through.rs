//! Rust counterpart of `c++/rpc/include/rpc/internal/pass_through.h`.

use std::sync::{Arc, Weak};

use crate::internal::error_codes;
use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
};
use crate::internal::service::Service;
use crate::internal::transport::Transport;
use crate::rpc_types::{DestinationZone, Zone};

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PassThroughKey {
    zone1: Zone,
    zone2: Zone,
}

impl PassThroughKey {
    pub fn new(zone1: Zone, zone2: Zone) -> Self {
        if zone1 <= zone2 {
            Self { zone1, zone2 }
        } else {
            Self {
                zone1: zone2,
                zone2: zone1,
            }
        }
    }
}

#[derive(Debug)]
pub struct PassThrough {
    forward: Arc<Transport>,
    reverse: Arc<Transport>,
    service: Weak<Service>,
    forward_dest: DestinationZone,
    reverse_dest: DestinationZone,
}

impl PassThrough {
    pub fn new(
        forward: Arc<Transport>,
        reverse: Arc<Transport>,
        service: Weak<Service>,
        forward_dest: DestinationZone,
        reverse_dest: DestinationZone,
    ) -> Self {
        Self {
            forward,
            reverse,
            service,
            forward_dest,
            reverse_dest,
        }
    }

    pub fn key(&self) -> PassThroughKey {
        PassThroughKey::new(self.forward_dest.clone(), self.reverse_dest.clone())
    }

    fn route_for_destination(&self, destination: &Zone) -> Option<&Transport> {
        if destination == &self.forward_dest {
            Some(self.forward.as_ref())
        } else if destination == &self.reverse_dest {
            Some(self.reverse.as_ref())
        } else {
            None
        }
    }

    pub fn transport_for_destination(&self, destination: &Zone) -> Option<Arc<Transport>> {
        if destination == &self.forward_dest {
            Some(self.forward.clone())
        } else if destination == &self.reverse_dest {
            Some(self.reverse.clone())
        } else {
            None
        }
    }

    fn route_for_remote_object(
        &self,
        remote_object: &crate::rpc_types::RemoteObject,
    ) -> Option<&Transport> {
        self.route_for_destination(&remote_object.as_zone())
    }

    pub fn service(&self) -> Option<Arc<Service>> {
        self.service.upgrade()
    }
}

impl IMarshaller for PassThrough {
    fn send(&self, params: SendParams) -> SendResult {
        self.route_for_remote_object(&params.remote_object_id)
            .map(|transport| transport.send(params.clone()))
            .unwrap_or_else(|| SendResult::new(error_codes::ZONE_NOT_FOUND(), vec![], vec![]))
    }

    fn post(&self, params: PostParams) {
        if let Some(transport) = self.route_for_remote_object(&params.remote_object_id) {
            transport.post(params);
        }
    }

    fn try_cast(&self, params: TryCastParams) -> StandardResult {
        self.route_for_remote_object(&params.remote_object_id)
            .map(|transport| transport.try_cast(params.clone()))
            .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]))
    }

    fn add_ref(&self, params: AddRefParams) -> StandardResult {
        self.route_for_remote_object(&params.remote_object_id)
            .map(|transport| transport.add_ref(params.clone()))
            .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]))
    }

    fn release(&self, params: ReleaseParams) -> StandardResult {
        self.route_for_remote_object(&params.remote_object_id)
            .map(|transport| transport.release(params.clone()))
            .unwrap_or_else(|| StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]))
    }

    fn object_released(&self, params: ObjectReleasedParams) {
        if let Some(transport) = self.route_for_remote_object(&params.remote_object_id) {
            transport.object_released(params);
        }
    }

    fn transport_down(&self, params: TransportDownParams) {
        if let Some(transport) = self.route_for_destination(&params.destination_zone_id) {
            transport.transport_down(params);
        }
    }

    fn get_new_zone_id(&self, params: GetNewZoneIdParams) -> NewZoneIdResult {
        self.forward.get_new_zone_id(params)
    }
}
