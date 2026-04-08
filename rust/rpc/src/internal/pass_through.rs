//! Rust counterpart of `c++/rpc/include/rpc/internal/pass_through.h`.

use std::sync::{Arc, Weak};

use crate::internal::error_codes;
use crate::internal::marshaller::IMarshaller;
use crate::internal::marshaller_params::{
    AddRefParams, GetNewZoneIdParams, NewZoneIdResult, ObjectReleasedParams, PostParams,
    ReleaseParams, SendParams, SendResult, StandardResult, TransportDownParams, TryCastParams,
};
use crate::internal::runtime_traits::{ServiceRuntime, TransportRuntime};
use crate::rpc_types::{AddRefOptions, DestinationZone, Zone};

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

pub struct PassThrough {
    forward: Arc<dyn TransportRuntime>,
    reverse: Arc<dyn TransportRuntime>,
    service: Weak<dyn ServiceRuntime>,
    forward_dest: DestinationZone,
    reverse_dest: DestinationZone,
}

impl std::fmt::Debug for PassThrough {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PassThrough")
            .field("forward_dest", &self.forward_dest)
            .field("reverse_dest", &self.reverse_dest)
            .field("has_service", &self.service.strong_count())
            .finish_non_exhaustive()
    }
}

impl PassThrough {
    pub fn new(
        forward: Arc<dyn TransportRuntime>,
        reverse: Arc<dyn TransportRuntime>,
        service: Weak<dyn ServiceRuntime>,
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

    fn route_for_destination(&self, destination: &Zone) -> Option<&dyn TransportRuntime> {
        if destination == &self.forward_dest {
            Some(self.forward.as_ref())
        } else if destination == &self.reverse_dest {
            Some(self.reverse.as_ref())
        } else {
            None
        }
    }

    pub fn transport_for_destination(
        &self,
        destination: &Zone,
    ) -> Option<Arc<dyn TransportRuntime>> {
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
    ) -> Option<&dyn TransportRuntime> {
        self.route_for_destination(&remote_object.as_zone())
    }

    pub fn service(&self) -> Option<Arc<dyn ServiceRuntime>> {
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
        let build_caller_channel =
            !(params.build_out_param_channel & AddRefOptions::BUILD_CALLER_ROUTE).is_empty();
        let build_dest_channel =
            !(params.build_out_param_channel & AddRefOptions::BUILD_DESTINATION_ROUTE).is_empty()
                || params.build_out_param_channel == AddRefOptions::NORMAL
                || params.build_out_param_channel == AddRefOptions::OPTIMISTIC;

        let destination_transport = if build_dest_channel {
            match self.route_for_remote_object(&params.remote_object_id) {
                Some(transport) => Some(transport),
                None => return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]),
            }
        } else {
            None
        };

        let caller_transport = if build_caller_channel {
            match self.route_for_destination(&params.caller_zone_id) {
                Some(transport) => Some(transport),
                None => return StandardResult::new(error_codes::ZONE_NOT_FOUND(), vec![]),
            }
        } else {
            None
        };

        let mut out_back_channel = vec![];

        if let Some(transport) = destination_transport {
            let mut dest_params = params.clone();
            dest_params.build_out_param_channel =
                params.build_out_param_channel & !AddRefOptions::BUILD_CALLER_ROUTE;
            let dest_result = transport.add_ref(dest_params);
            if dest_result.error_code != error_codes::OK() {
                return dest_result;
            }
            out_back_channel.extend(dest_result.out_back_channel);
        }

        if let Some(transport) = caller_transport {
            let mut caller_params = params;
            caller_params.build_out_param_channel =
                caller_params.build_out_param_channel & !AddRefOptions::BUILD_DESTINATION_ROUTE;
            let caller_result = transport.add_ref(caller_params);
            if caller_result.error_code != error_codes::OK() {
                return caller_result;
            }
            out_back_channel.extend(caller_result.out_back_channel);
        }

        StandardResult::new(error_codes::OK(), out_back_channel)
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
