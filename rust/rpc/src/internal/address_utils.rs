//! Rust counterpart of `c++/rpc/include/rpc/internal/address_utils.h`.

use crate::rpc_types::{ZoneAddress, ZoneAddressArgs};

pub fn to_zone_address_args(addr: &ZoneAddress) -> ZoneAddressArgs {
    ZoneAddressArgs::new(
        addr.get_version(),
        addr.get_address_type(),
        addr.get_port(),
        addr.get_routing_prefix(),
        addr.get_subnet_size_bits(),
        addr.get_subnet(),
        addr.get_object_id_size_bits(),
        addr.get_object_id(),
        Vec::new(),
    )
}

pub fn to_zone_address(args: ZoneAddressArgs) -> Result<ZoneAddress, String> {
    ZoneAddress::create(args)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpc_types::{AddressType, DefaultValues};

    #[test]
    fn conversion_round_trips_without_validation_bits() {
        let args = ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Ipv4,
            4040,
            vec![127, 0, 0, 1],
            32,
            9,
            16,
            99,
            vec![],
        );

        let addr = to_zone_address(args.clone()).expect("zone address should be created");
        let round_trip = to_zone_address_args(&addr);

        assert_eq!(round_trip.version, args.version);
        assert_eq!(round_trip.r#type, args.r#type);
        assert_eq!(round_trip.port, args.port);
        assert_eq!(round_trip.routing_prefix, args.routing_prefix);
        assert_eq!(round_trip.subnet_size_bits, args.subnet_size_bits);
        assert_eq!(round_trip.subnet, args.subnet);
        assert_eq!(round_trip.object_id_size_bits, args.object_id_size_bits);
        assert_eq!(round_trip.object_id, args.object_id);
        assert!(round_trip.validation_bits.is_empty());
    }
}
