# Canopy project GDB initialisation
# GDB will load this automatically when started from the project root,
# provided auto-load is enabled in ~/.gdbinit:
#
#   add-auto-load-safe-path /var/home/edward/projects/Canopy/.gdbinit
#
# Or to trust all project .gdbinit files:
#   set auto-load safe-path /

source tools/gdb/canopy_printers.py
