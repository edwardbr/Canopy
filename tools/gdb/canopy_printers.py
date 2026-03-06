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


def _unwrap_value(val):
    if val.type.code == gdb.TYPE_CODE_REF:
        val = val.referenced_value()
    return val


def _array_to_bytes(array_val):
    array_val = _unwrap_value(array_val)
    ptr = array_val.address
    if ptr is None:
        raise gdb.MemoryError("array value has no address (not in memory)")
    size = int(array_val.type.strip_typedefs().sizeof)
    mem = bytes(gdb.selected_inferior().read_memory(int(ptr), size))
    return list(mem)


def _bits_from_bytes(byte_list, begin, end):
    if end <= begin:
        return 0

    width = min(end - begin, 64)
    value = 0
    byte_count = len(byte_list)
    for i in range(width):
        bit_index = begin + i
        byte_index = (byte_count - 1) - (bit_index // 8)
        bit_in_byte = bit_index % 8
        bit = (byte_list[byte_index] >> bit_in_byte) & 0x1
        value |= bit << i
    return value


def _prefix64_from_host_bytes(byte_list):
    prefix = 0
    for b in byte_list[:8]:
        prefix = (prefix << 8) | b
    return prefix


def _read_zone_address(val):
    """
    Read a zone_address value.

    Returns a dict with decoded layout-specific fields.
    """
    val = _unwrap_value(val)

    fields = {f.name for f in val.type.strip_typedefs().fields()}

    if 'routing_prefix' in fields:
        routing_prefix = int(val['routing_prefix'])
        host_bytes = [0] * 16
        for i in range(min(8, len(host_bytes))):
            host_bytes[7 - i] = (routing_prefix >> (i * 8)) & 0xFF
        return {
            "layout": "fixed",
            "routing_prefix": routing_prefix,
            "subnet": int(val['subnet_id']),
            "object_id": int(val['object_id']),
            "host_address": host_bytes,
            "object_offset": None,
            "local_address": None,
        }

    host_bytes = _array_to_bytes(val['host_address'])
    local_bytes = _array_to_bytes(val['local_address'])
    object_offset = int(val['object_offset_'])

    return {
        "layout": "flex",
        "routing_prefix": _prefix64_from_host_bytes(host_bytes),
        "subnet": _bits_from_bytes(local_bytes, 0, object_offset),
        "object_id": _bits_from_bytes(local_bytes, object_offset, len(local_bytes) * 8),
        "host_address": host_bytes,
        "object_offset": object_offset,
        "local_address": local_bytes,
    }


class ZoneAddressPrinter:
    """Pretty-printer for rpc::zone_address (both fixed and flexible layouts)."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            data = _read_zone_address(self.val)
            rp = data["routing_prefix"]
            sn = data["subnet"]
            oid = data["object_id"]
            layout = data["layout"]
            if rp == 0 and oid == 0:
                return f"zone_address({layout}) subnet={sn}"
            return f"zone_address({layout}) prefix={rp} subnet={sn} object={oid}"
        except Exception as e:
            return f"zone_address(<error: {e}>)"

    def children(self):
        try:
            data = _read_zone_address(self.val)
            yield ("routing_prefix", data["routing_prefix"])
            yield ("subnet", data["subnet"])
            yield ("object_id", data["object_id"])
            if data["host_address"] is not None:
                yield ("host_address", "[" + " ".join(f"{b:02x}" for b in data["host_address"]) + "]")
            if data["object_offset"] is not None:
                yield ("object_offset", data["object_offset"])
            if data["local_address"] is not None:
                yield ("local_address", "[" + " ".join(f"{b:02x}" for b in data["local_address"]) + "]")
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
    """Print a rpc::zone_address, decoding routing_prefix / subnet / object_id.

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
            rp = data["routing_prefix"]
            sn = data["subnet"]
            oid = data["object_id"]
            layout = data["layout"]
            print(f"zone_address [{layout}]")
            print(f"  routing_prefix : {rp}")
            print(f"  subnet         : {sn}")
            print(f"  object_id      : {oid}")
            if data["host_address"] is not None:
                print("  host_address   : " + " ".join(f"{b:02x}" for b in data["host_address"]))
            if data["object_offset"] is not None:
                print(f"  object_offset  : {data['object_offset']}")
            if data["local_address"] is not None:
                print("  local_address  : " + " ".join(f"{b:02x}" for b in data["local_address"]))
        except Exception as e:
            print(f"pza error: {e}")


PrintZoneAddress()
