#include <arpa/inet.h>
#include <rpc/rpc.h>
#include <arpa/inet.h>
#include "address_translator.h"

namespace websocket_demo
{
    namespace v1
    {
        object_address to_object_address(const rpc::zone_address& addr)
        {
            object_address result;
            auto host = addr.get_routing_prefix();
            host.resize(16, 0);
            char buf[INET6_ADDRSTRLEN] = {};
            inet_ntop(AF_INET6, host.data(), buf, sizeof(buf));
            result.routing_prefix = buf;
            result.subnet = addr.get_subnet();
            result.object_id = addr.get_object_id();
            return result;
        }

        rpc::zone_address to_zone_address(const object_address& addr)
        {
            std::vector<uint8_t> host(16, 0);
            inet_pton(AF_INET6, addr.routing_prefix.c_str(), host.data());
            return *rpc::zone_address::create(rpc::zone_address::construction_args(rpc::zone_address::version_3,
                rpc::zone_address::address_type::ipv6,
                0,
                host,
                64,
                addr.subnet,
                56,
                addr.object_id,
                {}));
        }
    }
}
