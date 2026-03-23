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
"""

import gdb
import gdb.printing


ZONE_ADDRESS_VERSION_3 = 3
ZONE_ADDRESS_TYPE_LOCAL = 0
ZONE_ADDRESS_TYPE_IPV4 = 1
ZONE_ADDRESS_TYPE_IPV6 = 2
ZONE_ADDRESS_TYPE_IPV6_TUN = 3
ZONE_ADDRESS_HAS_PORT_MASK = 0x8


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


def _read_zone_address(val):
    val = _unwrap_value(val)
    blob = _vector_to_bytes(val["blob"])
    capability_blob = blob[:6]
    version = _bits_from_le_bytes(blob, 0, 8)
    capability_header = _bits_from_le_bytes(blob, 8, 8)
    type_code = capability_header & 0x7
    has_port = (capability_header & ZONE_ADDRESS_HAS_PORT_MASK) != 0
    subnet_size_bits = _bits_from_le_bytes(blob, 16, 8)
    object_id_size_bits = _bits_from_le_bytes(blob, 24, 8)
    validation_size_bits = _bits_from_le_bytes(blob, 32, 16)
    address_bits = 0
    if type_code == ZONE_ADDRESS_TYPE_IPV4:
        address_bits = 32
    elif type_code in (ZONE_ADDRESS_TYPE_IPV6, ZONE_ADDRESS_TYPE_IPV6_TUN):
        address_bits = 128

    address_offset = 48 + (16 if has_port else 0)
    address_byte_offset = address_offset // 8
    address_byte_count = address_bits // 8
    host_bytes = blob[address_byte_offset:address_byte_offset + address_byte_count]
    subnet_offset = address_offset + address_bits
    object_offset = subnet_offset + subnet_size_bits
    if type_code == ZONE_ADDRESS_TYPE_IPV6_TUN:
        object_id = _bits_from_be_bytes(host_bytes, 128 - object_id_size_bits, object_id_size_bits)
        subnet = _bits_from_be_bytes(host_bytes, 128 - object_id_size_bits - subnet_size_bits, subnet_size_bits)
        validation_offset = address_offset + 128
        routing_prefix_bits = 128 - subnet_size_bits - object_id_size_bits
        routing_prefix = _bits_from_be_bytes(host_bytes, 0, routing_prefix_bits)
    else:
        subnet = _bits_from_le_bytes(blob, subnet_offset, subnet_size_bits)
        object_id = _bits_from_le_bytes(blob, object_offset, object_id_size_bits)
        validation_offset = object_offset + object_id_size_bits
        if type_code == ZONE_ADDRESS_TYPE_IPV4:
            routing_prefix = _bits_from_be_bytes(host_bytes, 0, 32)
        elif type_code == ZONE_ADDRESS_TYPE_IPV6:
            routing_prefix = _bits_from_be_bytes(host_bytes, 0, 64)
        else:
            routing_prefix = 0

    validation_bytes = blob[validation_offset // 8:(validation_offset + validation_size_bits + 7) // 8]
    return {
        "version": version,
        "capability_blob": capability_blob,
        "capability_header": capability_header,
        "type_code": type_code,
        "type_name": _zone_address_type_name(type_code),
        "has_port": has_port,
        "port": _bits_from_le_bytes(blob, 48, 16) if has_port else 0,
        "routing_prefix": routing_prefix,
        "subnet": subnet,
        "object_id": object_id,
        "subnet_size_bits": subnet_size_bits,
        "object_id_size_bits": object_id_size_bits,
        "validation_size_bits": validation_size_bits,
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
            return (
                f"zone_address(v={data['version']} type={data['type_name']} "
                f"capability_bits={_format_hex_bytes(data['capability_blob'])} port={data['port']} "
                f"prefix={data['routing_prefix']} subnet={data['subnet']} object={data['object_id']})"
            )
        except Exception as e:
            return f"zone_address(<error: {e}>)"

    def children(self):
        try:
            data = _read_zone_address(self.val)
            yield ("version", data["version"])
            yield ("capability_bits", _format_hex_bytes(data["capability_blob"]))
            yield ("capability_header", f"0x{data['capability_header']:02x}")
            yield ("type", data["type_name"])
            yield ("has_port", data["has_port"])
            yield ("port", data["port"])
            yield ("routing_prefix", data["routing_prefix"])
            yield ("subnet", data["subnet"])
            yield ("object_id", data["object_id"])
            yield ("subnet_size_bits", data["subnet_size_bits"])
            yield ("object_id_size_bits", data["object_id_size_bits"])
            yield ("validation_size_bits", data["validation_size_bits"])
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
            print("zone_address [v3 blob]")
            print(f"  version            : {data['version']}")
            print(f"  capability_bits    : {_format_hex_bytes(data['capability_blob'])}")
            print(f"  capability_header  : 0x{data['capability_header']:02x}")
            print(f"  type               : {data['type_name']}")
            print(f"  has_port           : {data['has_port']}")
            print(f"  port               : {data['port']}")
            print(f"  routing_prefix     : {data['routing_prefix']}")
            print(f"  subnet             : {data['subnet']}")
            print(f"  object_id          : {data['object_id']}")
            print(f"  subnet_size_bits   : {data['subnet_size_bits']}")
            print(f"  object_id_size_bits: {data['object_id_size_bits']}")
            print(f"  validation_size    : {data['validation_size_bits']}")
            print("  host_address       : " + " ".join(f"{b:02x}" for b in data["host_address"]))
            print("  validation         : " + " ".join(f"{b:02x}" for b in data["validation"]))
            print("  blob               : " + " ".join(f"{b:02x}" for b in data["blob"]))
        except Exception as e:
            print(f"pza error: {e}")


PrintZoneAddress()
