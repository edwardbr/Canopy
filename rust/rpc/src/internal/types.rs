//! Rust counterpart of `c++/rpc/include/rpc/internal/types.h`.

use crate::rpc_types::{InterfaceOrdinal, Method, Object, RemoteObject, Zone, ZoneAddress};

pub use crate::rpc_types::CallerZone;
pub use crate::rpc_types::DestinationZone;
pub use crate::rpc_types::RequestingZone;

pub fn bytes_to_string(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(".")
}

pub fn zone_address_to_string(value: &ZoneAddress) -> String {
    let routing_prefix = value.get_routing_prefix();
    if routing_prefix.is_empty() && value.get_object_id() == 0 {
        value.get_subnet().to_string()
    } else {
        format!(
            "{}:{}/{}",
            bytes_to_string(&routing_prefix),
            value.get_subnet(),
            value.get_object_id()
        )
    }
}

pub fn zone_to_string(value: &Zone) -> String {
    zone_address_to_string(value.get_address())
}

pub fn remote_object_to_string(value: &RemoteObject) -> String {
    zone_address_to_string(value.get_address())
}

pub fn object_to_string(value: Object) -> String {
    value.get_val().to_string()
}

pub fn interface_ordinal_to_string(value: InterfaceOrdinal) -> String {
    value.get_val().to_string()
}

pub fn method_to_string(value: Method) -> String {
    value.get_val().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpc_types::{AddressType, DefaultValues, ZoneAddressArgs};

    #[test]
    fn bytes_to_string_matches_cpp_shape() {
        assert_eq!(bytes_to_string(&[]), "");
        assert_eq!(bytes_to_string(&[0x01, 0xab, 0xff]), "01.ab.ff");
    }

    #[test]
    fn zone_address_to_string_uses_subnet_only_for_local_without_object() {
        let addr = ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Local,
            0,
            vec![],
            64,
            42,
            64,
            0,
            vec![],
        ))
        .expect("local address should be valid");

        assert_eq!(zone_address_to_string(&addr), "42");
    }
}
