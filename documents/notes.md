get_val should return the whole address
zone_address should be configurable between ip v6, v4 with local address suffix, or none.
The size of the subnet should be configurable between 0 to 64 bits
The size of the object should be configurable between 0 to 64 bits
the object id can either be incorprated into the ipv6 address or in its own 64bit addresss

update the telemetry to support destination id with objectid incorporated
❯ if you use clang format on idls you will need to replace all instances of "#cpp_quote(R ^ __(" back to "#cpp_quote(R^__(" as it does not understand them.  