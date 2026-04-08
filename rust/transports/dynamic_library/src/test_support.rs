#![cfg(test)]

use canopy_rpc::{
    AddressType, DefaultValues, Object, RemoteObject, Zone, ZoneAddress, ZoneAddressArgs,
};

pub fn sample_zone_address_with(port: u16, subnet: u64, object_id: u64) -> ZoneAddress {
    ZoneAddress::create(ZoneAddressArgs::new(
        DefaultValues::VERSION_3,
        AddressType::Ipv4,
        port,
        vec![127, 0, 0, 1],
        32,
        subnet,
        16,
        object_id,
        vec![],
    ))
    .expect("sample zone address should be valid")
}

pub fn sample_zone_with(port: u16, subnet: u64, object_id: u64) -> Zone {
    Zone::new(sample_zone_address_with(port, subnet, object_id))
}

pub fn sample_zone() -> Zone {
    sample_zone_with(8080, 7, 9)
}

pub fn sample_remote_object() -> RemoteObject {
    sample_zone()
        .with_object(Object::new(42))
        .expect("with_object should succeed")
}
