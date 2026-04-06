//! Handwritten Rust-facing layer for the shared RPC types contract.
//!
//! This will track the role of the generated/public C++ `rpc_types` surface
//! while staying aligned with `interfaces/rpc/rpc_types.idl`.

use std::cmp::Ordering;
use std::ops::{BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, Not};
use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum Encoding {
    YasBinary = 1,
    YasCompressedBinary = 2,
    YasJson = 8,
    ProtocolBuffers = 16,
}

impl Default for Encoding {
    fn default() -> Self {
        Self::ProtocolBuffers
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(transparent)]
pub struct AddRefOptions(pub u8);

impl AddRefOptions {
    pub const NORMAL: Self = Self(0);
    pub const BUILD_DESTINATION_ROUTE: Self = Self(1);
    pub const BUILD_CALLER_ROUTE: Self = Self(2);
    pub const OPTIMISTIC: Self = Self(4);

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl BitOr for AddRefOptions {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl BitOrAssign for AddRefOptions {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl BitAnd for AddRefOptions {
    type Output = Self;

    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl BitAndAssign for AddRefOptions {
    fn bitand_assign(&mut self, rhs: Self) {
        self.0 &= rhs.0;
    }
}

impl BitXor for AddRefOptions {
    type Output = Self;

    fn bitxor(self, rhs: Self) -> Self::Output {
        Self(self.0 ^ rhs.0)
    }
}

impl BitXorAssign for AddRefOptions {
    fn bitxor_assign(&mut self, rhs: Self) {
        self.0 ^= rhs.0;
    }
}

impl Not for AddRefOptions {
    type Output = Self;

    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(transparent)]
pub struct ReleaseOptions(pub u8);

impl ReleaseOptions {
    pub const NORMAL: Self = Self(0);
    pub const OPTIMISTIC: Self = Self(1);

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl BitOr for ReleaseOptions {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl BitOrAssign for ReleaseOptions {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl BitAnd for ReleaseOptions {
    type Output = Self;

    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl BitAndAssign for ReleaseOptions {
    fn bitand_assign(&mut self, rhs: Self) {
        self.0 &= rhs.0;
    }
}

impl BitXor for ReleaseOptions {
    type Output = Self;

    fn bitxor(self, rhs: Self) -> Self::Output {
        Self(self.0 ^ rhs.0)
    }
}

impl BitXorAssign for ReleaseOptions {
    fn bitxor_assign(&mut self, rhs: Self) {
        self.0 ^= rhs.0;
    }
}

impl Not for ReleaseOptions {
    type Output = Self;

    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct BackChannelEntry {
    pub type_id: u64,
    pub payload: Vec<u8>,
}

pub struct DefaultValues;

impl DefaultValues {
    pub const CAPABILITY_BLOB_BYTES: u8 = 4;
    pub const VERSION_3: u8 = 3;
    pub const DEFAULT_SUBNET_SIZE_BITS: u8 = 64;
    pub const DEFAULT_OBJECT_ID_SIZE_BITS: u8 = 64;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum AddressType {
    Local = 0,
    Ipv4 = 1,
    Ipv6 = 2,
    Ipv6Tun = 3,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneAddressArgs {
    pub version: u8,
    pub r#type: AddressType,
    pub port: u16,
    pub routing_prefix: Vec<u8>,
    pub subnet_size_bits: u8,
    pub subnet: u64,
    pub object_id_size_bits: u8,
    pub object_id: u64,
    pub validation_bits: Vec<u8>,
}

impl Default for ZoneAddressArgs {
    fn default() -> Self {
        Self {
            version: DefaultValues::VERSION_3,
            r#type: AddressType::Local,
            port: 0,
            routing_prefix: Vec::new(),
            subnet_size_bits: DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
            subnet: 0,
            object_id_size_bits: 64,
            object_id: 0,
            validation_bits: Vec::new(),
        }
    }
}

impl ZoneAddressArgs {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        version: u8,
        r#type: AddressType,
        port: u16,
        routing_prefix: Vec<u8>,
        subnet_size_bits: u8,
        subnet: u64,
        object_id_size_bits: u8,
        object_id: u64,
        validation_bits: Vec<u8>,
    ) -> Self {
        Self {
            version,
            r#type,
            port,
            routing_prefix,
            subnet_size_bits,
            subnet,
            object_id_size_bits,
            object_id,
            validation_bits,
        }
    }
}

pub type ZoneAddressCapabilityBits = [u8; DefaultValues::CAPABILITY_BLOB_BYTES as usize];

#[derive(Debug, Clone, PartialEq, Eq, Default, Hash)]
pub struct ZoneAddress {
    blob: Vec<u8>,
}

impl ZoneAddress {
    pub fn new(blob: Vec<u8>) -> Self {
        Self { blob }
    }

    pub fn create(args: ZoneAddressArgs) -> Result<Self, String> {
        let caps = make_capability_bits(
            args.version,
            args.r#type,
            args.port != 0,
            !args.validation_bits.is_empty(),
            args.subnet_size_bits,
            args.object_id_size_bits,
        );
        Self::create_with_caps(
            caps,
            args.port,
            args.routing_prefix,
            args.subnet,
            args.object_id,
            args.validation_bits,
        )
    }

    pub fn create_with_caps(
        caps: ZoneAddressCapabilityBits,
        port: u16,
        routing_prefix: Vec<u8>,
        subnet: u64,
        object_id: u64,
        validation_bits: Vec<u8>,
    ) -> Result<Self, String> {
        validate_constructor_args(
            &caps,
            port,
            &routing_prefix,
            subnet,
            object_id,
            &validation_bits,
        )?;

        let mut result = Self { blob: Vec::new() };
        result.initialise_blob(
            &caps,
            port,
            &routing_prefix,
            subnet,
            object_id,
            &validation_bits,
        )?;
        Ok(result)
    }

    pub fn get_blob(&self) -> &[u8] {
        &self.blob
    }

    pub fn into_blob(self) -> Vec<u8> {
        self.blob
    }

    pub fn get_version(&self) -> u8 {
        if self.blob.is_empty() {
            0
        } else {
            get_bits_le(&self.blob, VERSION_OFFSET_BITS, VERSION_BITS) as u8
        }
    }

    pub fn get_address_type(&self) -> AddressType {
        if self.blob.is_empty() {
            AddressType::Local
        } else {
            match (get_bits_le(&self.blob, ADDRESS_TYPE_OFFSET_BITS, ADDRESS_TYPE_BITS) as u8)
                & ADDRESS_TYPE_MASK
            {
                0 => AddressType::Local,
                1 => AddressType::Ipv4,
                2 => AddressType::Ipv6,
                3 => AddressType::Ipv6Tun,
                _ => AddressType::Local,
            }
        }
    }

    pub fn get_capability_bits(&self) -> ZoneAddressCapabilityBits {
        let mut bits = [0; DefaultValues::CAPABILITY_BLOB_BYTES as usize];
        for (dst, src) in bits.iter_mut().zip(self.blob.iter().copied()) {
            *dst = src;
        }
        bits
    }

    pub fn has_port(&self) -> bool {
        capability_has_port(&self.get_capability_bits())
    }

    pub fn get_port(&self) -> u16 {
        if !self.has_port() {
            0
        } else {
            get_bits_le(&self.blob, HEADER_BITS, PORT_BITS) as u16
        }
    }

    pub fn get_subnet_size_bits(&self) -> u8 {
        if self.blob.is_empty() {
            0
        } else {
            get_bits_le(&self.blob, SUBNET_SIZE_OFFSET_BITS, SIZE_FIELD_BITS) as u8
        }
    }

    pub fn get_object_id_size_bits(&self) -> u8 {
        if self.blob.is_empty() {
            0
        } else {
            get_bits_le(&self.blob, OBJECT_ID_SIZE_OFFSET_BITS, SIZE_FIELD_BITS) as u8
        }
    }

    pub fn has_validation(&self) -> bool {
        capability_has_validation(&self.get_capability_bits())
    }

    pub fn get_validation_size_bytes(&self) -> u32 {
        if self.blob.is_empty() {
            return 0;
        }

        let offset_bytes = (self.validation_offset_bits() / 8) as usize;
        if self.blob.len() <= offset_bytes {
            0
        } else {
            (self.blob.len() - offset_bytes) as u32
        }
    }

    pub fn get_routing_prefix(&self) -> Vec<u8> {
        match self.get_address_type() {
            AddressType::Local => Vec::new(),
            AddressType::Ipv4 => self.read_host_bytes()[..4].to_vec(),
            AddressType::Ipv6 => self.read_host_bytes(),
            AddressType::Ipv6Tun => {
                let host = self.read_host_bytes();
                let routing_bits = 128u16
                    - self.get_subnet_size_bits() as u16
                    - self.get_object_id_size_bits() as u16;
                let routing_bytes = routing_bits.div_ceil(8) as usize;
                let mut prefix = host[..routing_bytes].to_vec();
                let unused_bits = (routing_bytes * 8) as u8 - routing_bits as u8;
                if let Some(last) = prefix.last_mut() {
                    if unused_bits != 0 {
                        *last &= 0xffu8 << unused_bits;
                    }
                }
                prefix
            }
        }
    }

    pub fn get_subnet(&self) -> u64 {
        let subnet_bits = self.get_subnet_size_bits();
        if subnet_bits == 0 {
            return 0;
        }

        if self.get_address_type() == AddressType::Ipv6Tun {
            let host = self.read_host_bytes();
            let start = 128u16 - self.get_object_id_size_bits() as u16 - subnet_bits as u16;
            get_bits_be(&host, start, subnet_bits as u16)
        } else {
            get_bits_le(&self.blob, self.subnet_offset_bits(), subnet_bits as u16)
        }
    }

    pub fn get_object_id(&self) -> u64 {
        let object_bits = self.get_object_id_size_bits();
        if object_bits == 0 {
            return 0;
        }

        if self.get_address_type() == AddressType::Ipv6Tun {
            let host = self.read_host_bytes();
            let start = 128u16 - object_bits as u16;
            get_bits_be(&host, start, object_bits as u16)
        } else {
            get_bits_le(&self.blob, self.object_offset_bits(), object_bits as u16)
        }
    }

    pub fn set_subnet(&mut self, value: u64) -> Result<(), String> {
        let subnet_bits = self.get_subnet_size_bits();
        if subnet_bits == 0 {
            return if value == 0 {
                Ok(())
            } else {
                Err("subnet value is non-zero but subnet_size_bits is 0".to_string())
            };
        }

        if subnet_bits < 64 && value >= (1u64 << subnet_bits) {
            return Err("subnet value does not fit in subnet_size_bits".to_string());
        }

        if self.get_address_type() == AddressType::Ipv6Tun {
            let mut host = self.read_host_bytes();
            let start = 128u16 - self.get_object_id_size_bits() as u16 - subnet_bits as u16;
            if !set_bits_be(&mut host, start, subnet_bits as u16, value) {
                return Err("subnet value does not fit in subnet_size_bits".to_string());
            }
            self.write_host_bytes(&host);
            Ok(())
        } else {
            let offset = self.subnet_offset_bits();
            if set_bits_le(&mut self.blob, offset, subnet_bits as u16, value) {
                Ok(())
            } else {
                Err("subnet value does not fit in subnet_size_bits".to_string())
            }
        }
    }

    pub fn set_object_id(&mut self, value: u64) -> Result<(), String> {
        let object_bits = self.get_object_id_size_bits();
        if object_bits == 0 {
            return if value == 0 {
                Ok(())
            } else {
                Err("object_id value is non-zero but object_id_size_bits is 0".to_string())
            };
        }

        if object_bits < 64 && value >= (1u64 << object_bits) {
            return Err("object_id value does not fit in object_id_size_bits".to_string());
        }

        if self.get_address_type() == AddressType::Ipv6Tun {
            let mut host = self.read_host_bytes();
            let start = 128u16 - object_bits as u16;
            if !set_bits_be(&mut host, start, object_bits as u16, value) {
                return Err("object_id value does not fit in object_id_size_bits".to_string());
            }
            self.write_host_bytes(&host);
            Ok(())
        } else {
            let offset = self.object_offset_bits();
            if set_bits_le(&mut self.blob, offset, object_bits as u16, value) {
                Ok(())
            } else {
                Err("object_id value does not fit in object_id_size_bits".to_string())
            }
        }
    }

    pub fn zone_only(&self) -> Self {
        let mut copy = self.clone();
        let _ = copy.set_object_id(0);
        copy.clear_validation_bits();
        copy
    }

    pub fn with_object(&self, object_id: u64) -> Result<Self, String> {
        let mut copy = self.zone_only();
        copy.set_object_id(object_id)?;
        Ok(copy)
    }

    pub fn same_zone(&self, other: &Self) -> bool {
        if self.get_address_type() != other.get_address_type()
            || self.has_port() != other.has_port()
            || self.get_port() != other.get_port()
            || self.get_subnet_size_bits() != other.get_subnet_size_bits()
            || self.get_object_id_size_bits() != other.get_object_id_size_bits()
            || self.get_subnet() != other.get_subnet()
        {
            return false;
        }

        let mut lhs_host = self.read_host_bytes();
        let mut rhs_host = other.read_host_bytes();
        if self.get_address_type() == AddressType::Ipv6Tun {
            let object_bits = self.get_object_id_size_bits() as u16;
            for i in 0..object_bits {
                let bit = 127u16 - i;
                let byte_index = (bit / 8) as usize;
                let bit_index = 7u8 - (bit % 8) as u8;
                let mask = 1u8 << bit_index;
                lhs_host[byte_index] &= !mask;
                rhs_host[byte_index] &= !mask;
            }
        }
        lhs_host == rhs_host
    }

    pub fn is_set(&self) -> bool {
        !self.get_routing_prefix().is_empty()
            || self.get_subnet() != 0
            || self.get_object_id() != 0
            || self.has_port()
    }

    fn initialise_blob(
        &mut self,
        caps: &ZoneAddressCapabilityBits,
        port: u16,
        routing_prefix: &[u8],
        subnet: u64,
        object_id: u64,
        validation_bits: &[u8],
    ) -> Result<(), String> {
        let address_type = capability_address_type(caps);
        let include_port = capability_has_port(caps);
        let subnet_bits = capability_subnet_size_bits(caps) as u32;
        let object_bits = capability_object_id_size_bits(caps) as u32;

        let mut total_bits = HEADER_BITS as u32;
        if include_port {
            total_bits += PORT_BITS as u32;
        }
        total_bits += address_bits_for_type(address_type) as u32;
        if address_type != AddressType::Ipv6Tun {
            total_bits += subnet_bits + object_bits;
        }
        total_bits += (validation_bits.len() * 8) as u32;

        self.blob = vec![0; total_bits.div_ceil(8) as usize];
        for (dst, src) in self.blob.iter_mut().zip(caps.iter().copied()) {
            *dst = src;
        }

        if include_port {
            let _ = set_bits_le(&mut self.blob, HEADER_BITS, PORT_BITS, port as u64);
        }

        match address_type {
            AddressType::Ipv4 | AddressType::Ipv6 => {
                let prefix =
                    build_fixed_width_prefix(routing_prefix, address_bits_for_type(address_type))?;
                self.write_host_bytes(&prefix);
            }
            AddressType::Ipv6Tun => {
                let routing_bits = 128u16 - subnet_bits as u16 - object_bits as u16;
                let host = build_tunnel_host(
                    routing_prefix,
                    routing_bits,
                    subnet_bits as u8,
                    subnet,
                    object_bits as u8,
                    object_id,
                )?;
                self.write_host_bytes(&host);
            }
            AddressType::Local => {}
        }

        if address_type != AddressType::Ipv6Tun {
            self.set_subnet(subnet)?;
            self.set_object_id(object_id)?;
        }

        if !validation_bits.is_empty() {
            let offset = (self.validation_offset_bits() / 8) as usize;
            self.blob[offset..offset + validation_bits.len()].copy_from_slice(validation_bits);
        }

        Ok(())
    }

    fn address_offset_bits(&self) -> u16 {
        HEADER_BITS + if self.has_port() { PORT_BITS } else { 0 }
    }

    fn subnet_offset_bits(&self) -> u16 {
        self.address_offset_bits() + address_bits_for_type(self.get_address_type())
    }

    fn object_offset_bits(&self) -> u16 {
        self.subnet_offset_bits() + self.get_subnet_size_bits() as u16
    }

    fn validation_offset_bits(&self) -> u16 {
        if self.get_address_type() == AddressType::Ipv6Tun {
            self.address_offset_bits() + 128
        } else {
            self.object_offset_bits() + self.get_object_id_size_bits() as u16
        }
    }

    fn read_host_bytes(&self) -> Vec<u8> {
        let mut host = vec![0; INTERNET_ADDRESS_BYTES as usize];
        let address_bits = address_bits_for_type(self.get_address_type());
        if self.blob.is_empty() || address_bits == 0 {
            return host;
        }

        let byte_offset = (self.address_offset_bits() / 8) as usize;
        let byte_count = (address_bits / 8) as usize;
        for i in 0..byte_count {
            if i < host.len() && byte_offset + i < self.blob.len() {
                host[i] = self.blob[byte_offset + i];
            }
        }
        host
    }

    fn write_host_bytes(&mut self, host: &[u8]) {
        let address_bits = address_bits_for_type(self.get_address_type());
        if address_bits == 0 {
            return;
        }

        let byte_offset = (self.address_offset_bits() / 8) as usize;
        let byte_count = (address_bits / 8) as usize;
        for i in 0..byte_count {
            if i < host.len() && byte_offset + i < self.blob.len() {
                self.blob[byte_offset + i] = host[i];
            }
        }
    }

    fn clear_validation_bits(&mut self) {
        if !self.has_validation() {
            return;
        }
        let new_size = (self.validation_offset_bits() / 8) as usize;
        self.blob.resize(new_size, 0);
        if self.blob.len() > 1 {
            self.blob[1] &= !HAS_VALIDATION_MASK;
        }
    }
}

impl PartialOrd for ZoneAddress {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for ZoneAddress {
    fn cmp(&self, other: &Self) -> Ordering {
        self.blob.cmp(&other.blob)
    }
}

macro_rules! id_wrapper {
    ($name:ident) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
        #[repr(transparent)]
        pub struct $name(u64);

        impl $name {
            pub const fn new(initial_id: u64) -> Self {
                Self(initial_id)
            }

            pub const fn get_val(self) -> u64 {
                self.0
            }

            pub const fn is_set(self) -> bool {
                self.0 != 0
            }
        }

        impl From<u64> for $name {
            fn from(value: u64) -> Self {
                Self(value)
            }
        }
    };
}

id_wrapper!(Object);
id_wrapper!(InterfaceOrdinal);
id_wrapper!(Method);

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Zone {
    addr: ZoneAddress,
}

impl Zone {
    pub fn new(addr: ZoneAddress) -> Self {
        Self {
            addr: addr.zone_only(),
        }
    }

    pub fn get_address(&self) -> &ZoneAddress {
        &self.addr
    }

    pub fn into_address(self) -> ZoneAddress {
        self.addr
    }

    pub fn is_set(&self) -> bool {
        self.addr.is_set()
    }

    pub fn get_subnet(&self) -> u64 {
        self.addr.get_subnet()
    }

    pub fn with_object(&self, object: Object) -> Result<RemoteObject, String> {
        Ok(RemoteObject::new(self.addr.with_object(object.get_val())?))
    }
}

impl From<ZoneAddress> for Zone {
    fn from(value: ZoneAddress) -> Self {
        Self::new(value)
    }
}

pub type DestinationZone = Zone;
pub type CallerZone = Zone;
pub type RequestingZone = Zone;

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct RemoteObject {
    addr: ZoneAddress,
}

impl RemoteObject {
    pub fn new(addr: ZoneAddress) -> Self {
        Self { addr }
    }

    pub fn from_destination_zone(zone: &DestinationZone) -> Self {
        Self {
            addr: zone.get_address().clone(),
        }
    }

    pub fn get_address(&self) -> &ZoneAddress {
        &self.addr
    }

    pub fn into_address(self) -> ZoneAddress {
        self.addr
    }

    pub fn is_set(&self) -> bool {
        self.addr.is_set()
    }

    pub fn get_subnet(&self) -> u64 {
        self.addr.get_subnet()
    }

    pub fn get_object_id(&self) -> Object {
        Object::new(self.addr.get_object_id())
    }

    pub fn with_object(&self, object: Object) -> Result<Self, String> {
        Ok(Self::new(self.addr.with_object(object.get_val())?))
    }

    pub fn as_zone(&self) -> Zone {
        Zone::new(self.addr.clone())
    }
}

impl From<ZoneAddress> for RemoteObject {
    fn from(value: ZoneAddress) -> Self {
        Self::new(value)
    }
}

impl From<DestinationZone> for RemoteObject {
    fn from(value: DestinationZone) -> Self {
        Self::new(value.into_address())
    }
}

impl PartialEq<Zone> for RemoteObject {
    fn eq(&self, other: &Zone) -> bool {
        self.get_address().same_zone(other.get_address())
    }
}

impl PartialEq<RemoteObject> for Zone {
    fn eq(&self, other: &RemoteObject) -> bool {
        self.get_address().same_zone(other.get_address())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ConnectionSettings {
    pub inbound_interface_id: InterfaceOrdinal,
    pub outbound_interface_id: InterfaceOrdinal,
    pub remote_object_id: RemoteObject,
}

impl ConnectionSettings {
    pub fn get_object_id(&self) -> Object {
        self.remote_object_id.get_object_id()
    }
}

pub fn default_prefix() -> &'static ZoneAddress {
    static DEFAULT_PREFIX: OnceLock<ZoneAddress> = OnceLock::new();
    DEFAULT_PREFIX.get_or_init(|| {
        ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Local,
            0,
            vec![],
            DefaultValues::DEFAULT_SUBNET_SIZE_BITS,
            1,
            DefaultValues::DEFAULT_OBJECT_ID_SIZE_BITS,
            0,
            vec![],
        ))
        .expect("default local prefix should be valid")
    })
}

const INTERNET_ADDRESS_BYTES: u16 = 16;
const VERSION_BITS: u16 = 8;
const ADDRESS_TYPE_BITS: u16 = 3;
const HAS_PORT_BITS: u16 = 1;
const HAS_VALIDATION_BITS: u16 = 1;
const RESERVED_CAPABILITY_BITS: u16 = 3;
const SIZE_FIELD_BITS: u16 = 8;
const PORT_BITS: u16 = 16;
const VERSION_OFFSET_BITS: u16 = 0;
const ADDRESS_TYPE_OFFSET_BITS: u16 = VERSION_OFFSET_BITS + VERSION_BITS;
const SUBNET_SIZE_OFFSET_BITS: u16 = 16;
const OBJECT_ID_SIZE_OFFSET_BITS: u16 = 24;
const HEADER_BITS: u16 = DefaultValues::CAPABILITY_BLOB_BYTES as u16 * 8;
const ADDRESS_TYPE_MASK: u8 = 0x7;
const HAS_PORT_MASK: u8 = 0x8;
const HAS_VALIDATION_MASK: u8 = 0x10;

fn address_bits_for_type(address_type: AddressType) -> u16 {
    match address_type {
        AddressType::Local => 0,
        AddressType::Ipv4 => 32,
        AddressType::Ipv6 | AddressType::Ipv6Tun => 128,
    }
}

fn get_bits_le(data: &[u8], offset: u16, width: u16) -> u64 {
    if width == 0 {
        return 0;
    }

    let width = width.min(64);
    let mut value = 0u64;
    for i in 0..width {
        let bit = offset + i;
        let byte_index = (bit / 8) as usize;
        if byte_index >= data.len() {
            break;
        }

        let mask = 1u8 << (bit % 8);
        if data[byte_index] & mask != 0 {
            value |= 1u64 << i;
        }
    }
    value
}

fn set_bits_le(data: &mut Vec<u8>, offset: u16, width: u16, value: u64) -> bool {
    if width == 0 {
        return value == 0;
    }
    if width < 64 && value >= (1u64 << width) {
        return false;
    }

    let required_bits = offset as u32 + width as u32;
    let required_bytes = required_bits.div_ceil(8) as usize;
    if data.len() < required_bytes {
        data.resize(required_bytes, 0);
    }

    for i in 0..width {
        let bit = offset + i;
        let byte = &mut data[(bit / 8) as usize];
        let mask = 1u8 << (bit % 8);
        if ((value >> i) & 1) != 0 {
            *byte |= mask;
        } else {
            *byte &= !mask;
        }
    }
    true
}

fn get_bits_be(data: &[u8], offset: u16, width: u16) -> u64 {
    if width == 0 {
        return 0;
    }
    let width = width.min(64);
    let mut value = 0u64;
    for i in 0..width {
        let bit = offset + i;
        let byte_index = (bit / 8) as usize;
        let bit_index = 7u8 - (bit % 8) as u8;
        value <<= 1;
        value |= ((data[byte_index] >> bit_index) & 1) as u64;
    }
    value
}

fn set_bits_be(data: &mut [u8], offset: u16, width: u16, value: u64) -> bool {
    if width == 0 {
        return value == 0;
    }
    if width < 64 && value >= (1u64 << width) {
        return false;
    }

    for i in 0..width {
        let bit = offset + width - 1 - i;
        let byte_index = (bit / 8) as usize;
        let bit_index = 7u8 - (bit % 8) as u8;
        let mask = 1u8 << bit_index;
        if ((value >> i) & 1) != 0 {
            data[byte_index] |= mask;
        } else {
            data[byte_index] &= !mask;
        }
    }
    true
}

fn capability_bytes_to_vec(caps: &ZoneAddressCapabilityBits) -> Vec<u8> {
    caps.to_vec()
}

fn capability_version(caps: &ZoneAddressCapabilityBits) -> u8 {
    get_bits_le(
        &capability_bytes_to_vec(caps),
        VERSION_OFFSET_BITS,
        VERSION_BITS,
    ) as u8
}

fn capability_header_byte(caps: &ZoneAddressCapabilityBits) -> u8 {
    get_bits_le(
        &capability_bytes_to_vec(caps),
        ADDRESS_TYPE_OFFSET_BITS,
        ADDRESS_TYPE_BITS + HAS_PORT_BITS + HAS_VALIDATION_BITS + RESERVED_CAPABILITY_BITS,
    ) as u8
}

fn capability_address_type(caps: &ZoneAddressCapabilityBits) -> AddressType {
    match capability_header_byte(caps) & ADDRESS_TYPE_MASK {
        0 => AddressType::Local,
        1 => AddressType::Ipv4,
        2 => AddressType::Ipv6,
        3 => AddressType::Ipv6Tun,
        _ => AddressType::Local,
    }
}

fn capability_has_port(caps: &ZoneAddressCapabilityBits) -> bool {
    capability_header_byte(caps) & HAS_PORT_MASK != 0
}

fn capability_subnet_size_bits(caps: &ZoneAddressCapabilityBits) -> u8 {
    get_bits_le(
        &capability_bytes_to_vec(caps),
        SUBNET_SIZE_OFFSET_BITS,
        SIZE_FIELD_BITS,
    ) as u8
}

fn capability_object_id_size_bits(caps: &ZoneAddressCapabilityBits) -> u8 {
    get_bits_le(
        &capability_bytes_to_vec(caps),
        OBJECT_ID_SIZE_OFFSET_BITS,
        SIZE_FIELD_BITS,
    ) as u8
}

fn capability_has_validation(caps: &ZoneAddressCapabilityBits) -> bool {
    capability_header_byte(caps) & HAS_VALIDATION_MASK != 0
}

fn make_capability_bits(
    version: u8,
    address_type: AddressType,
    has_port: bool,
    has_validation: bool,
    subnet_size_bits: u8,
    object_id_size_bits: u8,
) -> ZoneAddressCapabilityBits {
    let mut bits = [0; DefaultValues::CAPABILITY_BLOB_BYTES as usize];
    let mut data = capability_bytes_to_vec(&bits);
    let bits_len = bits.len();
    let _ = set_bits_le(&mut data, VERSION_OFFSET_BITS, VERSION_BITS, version as u64);
    let _ = set_bits_le(
        &mut data,
        ADDRESS_TYPE_OFFSET_BITS,
        ADDRESS_TYPE_BITS + HAS_PORT_BITS + HAS_VALIDATION_BITS + RESERVED_CAPABILITY_BITS,
        (address_type as u8
            | if has_port { HAS_PORT_MASK } else { 0 }
            | if has_validation {
                HAS_VALIDATION_MASK
            } else {
                0
            }) as u64,
    );
    let _ = set_bits_le(
        &mut data,
        SUBNET_SIZE_OFFSET_BITS,
        SIZE_FIELD_BITS,
        subnet_size_bits as u64,
    );
    let _ = set_bits_le(
        &mut data,
        OBJECT_ID_SIZE_OFFSET_BITS,
        SIZE_FIELD_BITS,
        object_id_size_bits as u64,
    );
    bits.copy_from_slice(&data[..bits_len]);
    bits
}

fn validate_prefix_bits(data: &[u8], width: u16, field_name: &str) -> Result<(), String> {
    let required_bytes = width.div_ceil(8) as usize;
    if data.len() != required_bytes {
        return Err(format!("{field_name} has the wrong byte width"));
    }
    if width == 0 {
        return if data.is_empty() {
            Ok(())
        } else {
            Err(format!("{field_name} must be empty"))
        };
    }

    let leading_unused_bits = (required_bytes * 8) as u8 - width as u8;
    if leading_unused_bits == 0 {
        return Ok(());
    }
    let mask = 0xffu8 << (8 - leading_unused_bits);
    if data[0] & mask != 0 {
        Err(format!(
            "{field_name} does not fit in the declared bit width"
        ))
    } else {
        Ok(())
    }
}

fn build_fixed_width_prefix(data: &[u8], width: u16) -> Result<Vec<u8>, String> {
    validate_prefix_bits(data, width, "routing_prefix")?;
    let byte_width = width.div_ceil(8) as usize;
    let mut result = vec![0; byte_width];
    result[..data.len()].copy_from_slice(data);
    Ok(result)
}

fn build_tunnel_host(
    routing_prefix: &[u8],
    routing_bits: u16,
    subnet_size_bits: u8,
    subnet: u64,
    object_id_size_bits: u8,
    object_id: u64,
) -> Result<Vec<u8>, String> {
    let mut host = vec![0; INTERNET_ADDRESS_BYTES as usize];
    let prefix = build_fixed_width_prefix(routing_prefix, routing_bits)?;
    host[..prefix.len()].copy_from_slice(&prefix);
    if !set_bits_be(&mut host, routing_bits, subnet_size_bits as u16, subnet) {
        return Err("subnet does not fit in subnet_size_bits".to_string());
    }
    if !set_bits_be(
        &mut host,
        routing_bits + subnet_size_bits as u16,
        object_id_size_bits as u16,
        object_id,
    ) {
        return Err("object_id does not fit in object_id_size_bits".to_string());
    }
    Ok(host)
}

fn validate_constructor_args(
    caps: &ZoneAddressCapabilityBits,
    port: u16,
    routing_prefix: &[u8],
    subnet: u64,
    object_id: u64,
    validation_bits: &[u8],
) -> Result<(), String> {
    let version = capability_version(caps);
    let header_byte = capability_header_byte(caps);
    let address_type = capability_address_type(caps);
    let subnet_size_bits = capability_subnet_size_bits(caps);
    let object_id_size_bits = capability_object_id_size_bits(caps);
    let has_validation = capability_has_validation(caps);
    let include_port = capability_has_port(caps);

    if version != DefaultValues::VERSION_3 {
        return Err("zone_address only supports version 3".to_string());
    }
    if (header_byte & !(ADDRESS_TYPE_MASK | HAS_PORT_MASK | HAS_VALIDATION_MASK)) != 0 {
        return Err("zone_address capability bits use reserved values".to_string());
    }
    if !include_port && port != 0 {
        return Err("zone_address port provided without has_port capability".to_string());
    }
    if has_validation && validation_bits.is_empty() {
        return Err("zone_address has_validation set but no validation bytes provided".to_string());
    }
    if !has_validation && !validation_bits.is_empty() {
        return Err(
            "zone_address validation bytes provided without has_validation capability".to_string(),
        );
    }
    if subnet_size_bits > 64 {
        return Err("zone_address subnet_size_bits must be <= 64".to_string());
    }
    if object_id_size_bits > 64 {
        return Err("zone_address object_id_size_bits must be <= 64".to_string());
    }
    if subnet_size_bits < 64 && subnet_size_bits != 0 && subnet >= (1u64 << subnet_size_bits) {
        return Err("zone_address subnet does not fit in subnet_size_bits".to_string());
    }
    if object_id_size_bits < 64
        && object_id_size_bits != 0
        && object_id >= (1u64 << object_id_size_bits)
    {
        return Err("zone_address object_id does not fit in object_id_size_bits".to_string());
    }

    match address_type {
        AddressType::Local => {
            if include_port {
                return Err("local zone_address cannot contain a port".to_string());
            }
            if !routing_prefix.is_empty() {
                return Err("local zone_address cannot contain a routing prefix".to_string());
            }
            if !validation_bits.is_empty() {
                return Err("local zone_address cannot contain validation bits".to_string());
            }
        }
        AddressType::Ipv4 => validate_prefix_bits(routing_prefix, 32, "routing_prefix")?,
        AddressType::Ipv6 => validate_prefix_bits(routing_prefix, 128, "routing_prefix")?,
        AddressType::Ipv6Tun => {
            if subnet_size_bits as u16 + object_id_size_bits as u16 > 128 {
                return Err("ipv6_tun subnet/object bits exceed 128 bits".to_string());
            }
            let routing_bits = 128u16 - subnet_size_bits as u16 - object_id_size_bits as u16;
            validate_prefix_bits(routing_prefix, routing_bits, "routing_prefix")?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encoding_values_match_idl() {
        assert_eq!(Encoding::YasBinary as u64, 1);
        assert_eq!(Encoding::YasCompressedBinary as u64, 2);
        assert_eq!(Encoding::YasJson as u64, 8);
        assert_eq!(Encoding::ProtocolBuffers as u64, 16);
    }

    #[test]
    fn add_ref_flags_behave_like_bitflags() {
        let value = AddRefOptions::BUILD_CALLER_ROUTE | AddRefOptions::OPTIMISTIC;
        assert_eq!(value.0, 6);
        assert!(!(value & AddRefOptions::OPTIMISTIC).is_empty());
        assert!(AddRefOptions::NORMAL.is_empty());
    }

    #[test]
    fn release_flags_behave_like_bitflags() {
        let value = ReleaseOptions::NORMAL | ReleaseOptions::OPTIMISTIC;
        assert_eq!(value.0, 1);
        assert!(!(value & ReleaseOptions::OPTIMISTIC).is_empty());
    }

    #[test]
    fn id_wrappers_match_cpp_zero_is_unset_semantics() {
        assert!(!Object::default().is_set());
        assert!(Object::new(7).is_set());
        assert_eq!(InterfaceOrdinal::new(9).get_val(), 9);
        assert_eq!(Method::from(11).get_val(), 11);
    }

    #[test]
    fn zone_address_args_defaults_match_idl() {
        let args = ZoneAddressArgs::default();
        assert_eq!(args.version, DefaultValues::VERSION_3);
        assert_eq!(args.r#type, AddressType::Local);
        assert_eq!(
            args.subnet_size_bits,
            DefaultValues::DEFAULT_SUBNET_SIZE_BITS
        );
        assert_eq!(args.object_id_size_bits, 64);
    }

    #[test]
    fn zone_and_remote_object_wrap_zone_address() {
        let addr = ZoneAddress::new(vec![1, 2, 3]);
        let zone = Zone::new(addr.clone());
        let remote = RemoteObject::from_destination_zone(&zone);

        assert!(zone.is_set());
        assert!(remote.is_set());
        assert_eq!(zone.get_address(), &addr);
        assert_eq!(remote.get_address(), &addr);
    }

    #[test]
    fn local_zone_address_round_trips_basic_fields() {
        let addr = ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Local,
            0,
            vec![],
            64,
            7,
            64,
            11,
            vec![],
        ))
        .expect("local zone address should be valid");

        assert_eq!(addr.get_version(), DefaultValues::VERSION_3);
        assert_eq!(addr.get_address_type(), AddressType::Local);
        assert_eq!(addr.get_subnet(), 7);
        assert_eq!(addr.get_object_id(), 11);
        assert!(addr.is_set());
    }

    #[test]
    fn zone_only_and_with_object_match_cpp_intent() {
        let addr = ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Ipv4,
            8080,
            vec![127, 0, 0, 1],
            32,
            12,
            16,
            99,
            vec![1, 2, 3],
        ))
        .expect("ipv4 zone address should be valid");

        let zone_only = addr.zone_only();
        let with_object = zone_only
            .with_object(55)
            .expect("with_object should succeed");

        assert_eq!(zone_only.get_object_id(), 0);
        assert_eq!(zone_only.get_validation_size_bytes(), 0);
        assert_eq!(zone_only.get_subnet(), 12);
        assert_eq!(with_object.get_object_id(), 55);
        assert!(zone_only.same_zone(&with_object));
    }

    #[test]
    fn ipv6_tun_round_trips_embedded_subnet_and_object() {
        let addr = ZoneAddress::create(ZoneAddressArgs::new(
            DefaultValues::VERSION_3,
            AddressType::Ipv6Tun,
            0,
            vec![0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0],
            32,
            0x1234_5678,
            16,
            0xabcd,
            vec![],
        ))
        .expect("ipv6_tun address should be valid");

        assert_eq!(addr.get_address_type(), AddressType::Ipv6Tun);
        assert_eq!(addr.get_subnet(), 0x1234_5678);
        assert_eq!(addr.get_object_id(), 0xabcd);

        let changed = addr
            .with_object(0x2222)
            .expect("with_object should succeed");
        assert!(addr.same_zone(&changed));
        assert_eq!(changed.get_object_id(), 0x2222);
    }

    #[test]
    fn default_prefix_matches_generated_cpp_intent() {
        let prefix = default_prefix();
        assert_eq!(prefix.get_address_type(), AddressType::Local);
        assert_eq!(prefix.get_subnet(), 1);
        assert_eq!(prefix.get_object_id(), 0);
    }

    #[test]
    fn remote_object_and_zone_compare_by_same_zone() {
        let zone = Zone::new(
            ZoneAddress::create(ZoneAddressArgs::new(
                DefaultValues::VERSION_3,
                AddressType::Ipv4,
                8080,
                vec![127, 0, 0, 1],
                32,
                42,
                16,
                0,
                vec![],
            ))
            .expect("zone should be valid"),
        );
        let remote = zone
            .with_object(Object::new(99))
            .expect("with_object should succeed");

        assert_eq!(remote, zone);
        assert_eq!(zone, remote);
    }

    #[test]
    fn connection_settings_exposes_embedded_object_id() {
        let zone = Zone::from(default_prefix().clone());
        let remote = zone
            .with_object(Object::new(123))
            .expect("with_object should succeed");
        let settings = ConnectionSettings {
            inbound_interface_id: InterfaceOrdinal::new(1),
            outbound_interface_id: InterfaceOrdinal::new(2),
            remote_object_id: remote,
        };

        assert_eq!(settings.get_object_id(), Object::new(123));
    }
}
