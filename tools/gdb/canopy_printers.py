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


def _read_zone_address_mem(val):
    """
    Read a zone_address value via raw memory.

    Flexible layout (no CANOPY_FIXED_ADDRESS_SIZE):
        offset  0: unsigned __int128  address_       (16 bytes, little-endian)
        offset 16: uint8_t            subnet_offset_
        offset 17: uint8_t            object_offset_

    Fixed layout (CANOPY_FIXED_ADDRESS_SIZE defined):
        offset  0: uint64_t  routing_prefix  (8 bytes)
        offset  8: uint32_t  subnet_id       (4 bytes)
        offset 12: uint32_t  object_id       (4 bytes)

    Returns (routing_prefix, subnet, object_id, layout_name).
    """
    ptr = val.address
    if ptr is None:
        raise gdb.MemoryError("zone_address value has no address (not in memory)")

    inf = gdb.selected_inferior()

    # Detect layout by checking type field names.
    fields = {f.name for f in val.type.strip_typedefs().fields()}

    if 'routing_prefix' in fields:
        # Fixed layout: 8 + 4 + 4 = 16 bytes
        mem = bytes(inf.read_memory(int(ptr), 16))
        import struct as _struct
        rp, sn, oid = _struct.unpack_from('<QII', mem)
        return rp, sn, oid, "fixed"
    else:
        # Flexible layout: __int128 (16 bytes) + 2 x uint8
        mem = bytes(inf.read_memory(int(ptr), 18))
        address = int.from_bytes(mem[0:16], 'little')
        so = mem[16]
        oo = mem[17]
        rp  = (address & ((1 << so) - 1)) if so > 0 else 0
        sn  = ((address >> so) & ((1 << (oo - so)) - 1)) if oo > so else 0
        oid = (address >> oo) if oo > 0 else 0
        return rp, sn, oid, "flex"


class ZoneAddressPrinter:
    """Pretty-printer for rpc::zone_address (both fixed and flexible layouts)."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            rp, sn, oid, layout = _read_zone_address_mem(self.val)
            if rp == 0 and oid == 0:
                return f"zone_address({layout}) subnet={sn}"
            return f"zone_address({layout}) prefix={rp} subnet={sn} object={oid}"
        except Exception as e:
            return f"zone_address(<error: {e}>)"

    def children(self):
        try:
            rp, sn, oid, _ = _read_zone_address_mem(self.val)
            yield ("routing_prefix", rp)
            yield ("subnet",         sn)
            yield ("object_id",      oid)
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
            rp, sn, oid, layout = _read_zone_address_mem(val)
            print(f"zone_address [{layout}]")
            print(f"  routing_prefix : {rp}")
            print(f"  subnet         : {sn}")
            print(f"  object_id      : {oid}")
        except Exception as e:
            print(f"pza error: {e}")


PrintZoneAddress()
