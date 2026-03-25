"""
GDB pretty-printers for Canopy RPC types.

To load automatically, add this to your ~/.gdbinit:
    source /path/to/Canopy/tools/gdb/canopy_printers.py

Or add a project-local .gdbinit in the Canopy root:
    set auto-load safe-path /var/home/edward/projects/Canopy
    source tools/gdb/canopy_printers.py

    or add this in your "setupCommands" collection in launch.json

    {
        "description": "Load Canopy GDB pretty-printers",
        "text": "source ${workspaceFolder}/tools/gdb/canopy_printers.py",
        "ignoreFailures": true
    },

Also defines the 'pza' convenience command:
    pza my_zone_address
    pza some_struct.addr_

zone_address v3 blob layout (from rpc/interfaces/rpc/rpc_types.idl):

  Capability header — 4 bytes = 32 bits, little-endian packed:
    bits  0- 7  version              (8 bits)
    bits  8-10  address_type         (3 bits)  0=local 1=ipv4 2=ipv6 3=ipv6_tun
    bit  11     has_port             (1 bit)
    bit  12     has_validation       (1 bit)
    bits 13-15  reserved             (3 bits)
    bits 16-23  subnet_size_bits     (8 bits)
    bits 24-31  object_id_size_bits  (8 bits)

  Optional port — 16 bits LE, present only when has_port == 1
    bits 32-47  port

  Address payload — big-endian:
    local:    none (no routing prefix bytes)
    ipv4:     32 bits  (offset = 32 + 16*has_port)
    ipv6:     128 bits (offset = 32 + 16*has_port)
    ipv6_tun: 128 bits (offset = 32 + 16*has_port)

  Subnet and object_id — little-endian, follow address payload:
    local/ipv4/ipv6:
      subnet    at address_offset + address_bits
      object_id at subnet_offset + subnet_size_bits
    ipv6_tun:
      packed inside the 128-bit tunneled field (big-endian, from LSB):
        object_id occupies the least-significant object_id_size_bits
        subnet occupies the next subnet_size_bits above that

  Validation block — present only when has_validation == 1:
    appended at the end of the blob after object_id
    length = blob.size() - validation_offset_bytes
    first byte is a type discriminator; remaining bytes are the payload
"""

import gdb
import gdb.printing


ZONE_ADDRESS_VERSION_3 = 3
ZONE_ADDRESS_TYPE_LOCAL = 0
ZONE_ADDRESS_TYPE_IPV4 = 1
ZONE_ADDRESS_TYPE_IPV6 = 2
ZONE_ADDRESS_TYPE_IPV6_TUN = 3
ZONE_ADDRESS_HAS_PORT_MASK = 0x08        # bit 11 of blob = bit 3 of flags byte
ZONE_ADDRESS_HAS_VALIDATION_MASK = 0x10  # bit 12 of blob = bit 4 of flags byte
ZONE_ADDRESS_CAPABILITY_BYTES = 4        # capability header is exactly 4 bytes


def _unwrap_value(val):
    if val.type.code == gdb.TYPE_CODE_REF:
        val = val.referenced_value()
    return val


def _vector_to_bytes(vec_val):
    vec_val = _unwrap_value(vec_val)
    impl = vec_val["_M_impl"]
    start = int(impl["_M_start"])
    finish = int(impl["_M_finish"])
    if finish < start:
        return []
    size = finish - start
    if size == 0:
        return []
    mem = bytes(gdb.selected_inferior().read_memory(start, size))
    return list(mem)


def _bits_from_le_bytes(byte_list, begin, width):
    """Extract 'width' bits starting at bit offset 'begin' from little-endian packed bytes."""
    if width <= 0:
        return 0

    width = min(width, 64)
    value = 0
    for i in range(width):
        bit = begin + i
        byte_index = bit // 8
        if byte_index >= len(byte_list):
            break
        bit_in_byte = bit % 8
        value |= ((byte_list[byte_index] >> bit_in_byte) & 0x1) << i
    return value


def _bits_from_be_bytes(byte_list, begin, width):
    """Extract 'width' bits starting at bit offset 'begin' from big-endian packed bytes."""
    if width <= 0:
        return 0

    width = min(width, 64)
    value = 0
    for i in range(width):
        bit = begin + i
        byte_index = bit // 8
        if byte_index >= len(byte_list):
            break
        bit_in_byte = 7 - (bit % 8)
        value <<= 1
        value |= (byte_list[byte_index] >> bit_in_byte) & 0x1
    return value


def _zone_address_type_name(type_code):
    return {
        ZONE_ADDRESS_TYPE_LOCAL: "local",
        ZONE_ADDRESS_TYPE_IPV4: "ipv4",
        ZONE_ADDRESS_TYPE_IPV6: "ipv6",
        ZONE_ADDRESS_TYPE_IPV6_TUN: "ipv6_tun",
    }.get(type_code, f"unknown({type_code})")


def _format_hex_bytes(byte_list):
    return "[" + " ".join(f"{b:02x}" for b in byte_list) + "]"


def _format_ipv4(byte_list):
    if len(byte_list) < 4:
        return _format_hex_bytes(byte_list)
    return f"{byte_list[0]}.{byte_list[1]}.{byte_list[2]}.{byte_list[3]}"


def _format_ipv6(byte_list):
    if len(byte_list) < 16:
        return _format_hex_bytes(byte_list)
    groups = []
    for i in range(0, 16, 2):
        groups.append(f"{byte_list[i]:02x}{byte_list[i+1]:02x}")
    return ":".join(groups)


def _read_zone_address(val):
    val = _unwrap_value(val)
    blob = _vector_to_bytes(val["blob"])

    # --- Capability header (4 bytes / 32 bits, little-endian) ---
    capability_blob = blob[:ZONE_ADDRESS_CAPABILITY_BYTES]
    version = _bits_from_le_bytes(blob, 0, 8)
    flags_byte = _bits_from_le_bytes(blob, 8, 8)   # bits 8-15
    type_code = flags_byte & 0x7
    has_port = (flags_byte & ZONE_ADDRESS_HAS_PORT_MASK) != 0
    has_validation = (flags_byte & ZONE_ADDRESS_HAS_VALIDATION_MASK) != 0
    subnet_size_bits = _bits_from_le_bytes(blob, 16, 8)
    object_id_size_bits = _bits_from_le_bytes(blob, 24, 8)

    # --- Optional port (16 bits LE at bit 32, present only when has_port) ---
    port = _bits_from_le_bytes(blob, 32, 16) if has_port else 0

    # --- Address payload (big-endian, follows capability header + optional port) ---
    # Capability header is 32 bits; port (if present) adds 16 bits.
    address_offset = 32 + (16 if has_port else 0)  # in bits

    address_bits = 0
    if type_code == ZONE_ADDRESS_TYPE_IPV4:
        address_bits = 32
    elif type_code in (ZONE_ADDRESS_TYPE_IPV6, ZONE_ADDRESS_TYPE_IPV6_TUN):
        address_bits = 128

    address_byte_offset = address_offset // 8
    address_byte_count = address_bits // 8
    host_bytes = blob[address_byte_offset:address_byte_offset + address_byte_count]

    # --- Subnet, object_id, and routing_prefix ---
    if type_code == ZONE_ADDRESS_TYPE_IPV6_TUN:
        # For ipv6_tun the subnet and object_id are packed inside the 128-bit
        # tunneled field (big-endian from the LSB):
        #   object_id: least-significant object_id_size_bits
        #   subnet:    next subnet_size_bits above that
        #   routing prefix: remaining most-significant bits
        object_id = _bits_from_be_bytes(host_bytes, 128 - object_id_size_bits, object_id_size_bits)
        subnet = _bits_from_be_bytes(host_bytes, 128 - object_id_size_bits - subnet_size_bits, subnet_size_bits)
        routing_prefix_bits = 128 - subnet_size_bits - object_id_size_bits
        routing_prefix_int = _bits_from_be_bytes(host_bytes, 0, routing_prefix_bits)
        routing_prefix_str = _format_ipv6(host_bytes)
        validation_offset = address_offset + 128
    else:
        subnet_offset = address_offset + address_bits
        object_offset = subnet_offset + subnet_size_bits
        subnet = _bits_from_le_bytes(blob, subnet_offset, subnet_size_bits)
        object_id = _bits_from_le_bytes(blob, object_offset, object_id_size_bits)
        validation_offset = object_offset + object_id_size_bits
        if type_code == ZONE_ADDRESS_TYPE_IPV4:
            routing_prefix_int = _bits_from_be_bytes(host_bytes, 0, 32)
            routing_prefix_str = _format_ipv4(host_bytes)
        elif type_code == ZONE_ADDRESS_TYPE_IPV6:
            routing_prefix_int = _bits_from_be_bytes(host_bytes, 0, 64)
            routing_prefix_str = _format_ipv6(host_bytes)
        else:
            routing_prefix_int = 0
            routing_prefix_str = ""

    # --- Validation block (variable length, trailing bytes) ---
    validation_byte_offset = validation_offset // 8
    validation_bytes = blob[validation_byte_offset:] if has_validation else []

    return {
        "version": version,
        "capability_blob": capability_blob,
        "flags_byte": flags_byte,
        "type_code": type_code,
        "type_name": _zone_address_type_name(type_code),
        "has_port": has_port,
        "has_validation": has_validation,
        "port": port,
        "routing_prefix": routing_prefix_int,
        "routing_prefix_str": routing_prefix_str,
        "subnet": subnet,
        "object_id": object_id,
        "subnet_size_bits": subnet_size_bits,
        "object_id_size_bits": object_id_size_bits,
        "host_address": host_bytes,
        "blob": blob,
        "validation": validation_bytes,
    }


class ZoneAddressPrinter:
    """Pretty-printer for rpc::zone_address version-3 blob layout."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            data = _read_zone_address(self.val)
            prefix = data["routing_prefix_str"] or str(data["routing_prefix"])
            return (
                f"zone_address(v={data['version']} type={data['type_name']} "
                f"port={data['port']} prefix={prefix} "
                f"subnet={data['subnet']} object={data['object_id']})"
            )
        except Exception as e:
            return f"zone_address(<error: {e}>)"

    def children(self):
        try:
            data = _read_zone_address(self.val)
            yield ("version", data["version"])
            yield ("capability_bits", _format_hex_bytes(data["capability_blob"]))
            yield ("flags_byte", f"0x{data['flags_byte']:02x}")
            yield ("type", data["type_name"])
            yield ("has_port", data["has_port"])
            yield ("has_validation", data["has_validation"])
            yield ("port", data["port"])
            yield ("routing_prefix", data["routing_prefix_str"] or str(data["routing_prefix"]))
            yield ("subnet", data["subnet"])
            yield ("object_id", data["object_id"])
            yield ("subnet_size_bits", data["subnet_size_bits"])
            yield ("object_id_size_bits", data["object_id_size_bits"])
            yield ("host_address", _format_hex_bytes(data["host_address"]))
            yield ("validation", _format_hex_bytes(data["validation"]))
            yield ("blob", _format_hex_bytes(data["blob"]))
        except Exception:
            pass


def build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("canopy")
    pp.add_printer("zone_address", r"^rpc::zone_address$", ZoneAddressPrinter)
    return pp


gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer())


# ---------------------------------------------------------------------------
# 'pza' convenience command — works on any expression yielding zone_address
# ---------------------------------------------------------------------------

class PrintZoneAddress(gdb.Command):
    """Print a rpc::zone_address, decoding the version-3 blob fields.

Usage: pza EXPR
Example:
    pza my_zone_address
    pza some_struct.addr_
    pza *ptr_to_zone_address
"""

    def __init__(self):
        super().__init__("pza", gdb.COMMAND_DATA, gdb.COMPLETE_EXPRESSION)

    def invoke(self, arg, from_tty):
        if not arg.strip():
            print("Usage: pza EXPR")
            return
        try:
            val = gdb.parse_and_eval(arg.strip())
            data = _read_zone_address(val)
            prefix = data["routing_prefix_str"] or str(data["routing_prefix"])
            print("zone_address [v3 blob]")
            print(f"  version            : {data['version']}")
            print(f"  capability_bits    : {_format_hex_bytes(data['capability_blob'])}")
            print(f"  flags_byte         : 0x{data['flags_byte']:02x}")
            print(f"  type               : {data['type_name']}")
            print(f"  has_port           : {data['has_port']}")
            print(f"  has_validation     : {data['has_validation']}")
            print(f"  port               : {data['port']}")
            print(f"  routing_prefix     : {prefix}")
            print(f"  subnet             : {data['subnet']}")
            print(f"  object_id          : {data['object_id']}")
            print(f"  subnet_size_bits   : {data['subnet_size_bits']}")
            print(f"  object_id_size_bits: {data['object_id_size_bits']}")
            print("  host_address       : " + " ".join(f"{b:02x}" for b in data["host_address"]))
            if data["validation"]:
                print("  validation         : " + " ".join(f"{b:02x}" for b in data["validation"]))
            else:
                print("  validation         : (none)")
            print("  blob               : " + " ".join(f"{b:02x}" for b in data["blob"]))
        except Exception as e:
            print(f"pza error: {e}")


PrintZoneAddress()
