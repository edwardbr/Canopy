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
            auto host = addr.get_host_address();
            char buf[INET6_ADDRSTRLEN] = {};
            inet_ntop(AF_INET6, host.data(), buf, sizeof(buf));
            result.routing_prefix = buf;
            result.subnet = addr.get_subnet();
            result.object_id = addr.get_object_id();
            return result;
        }

        rpc::zone_address to_zone_address(const object_address& addr)
        {
            std::array<uint8_t, 16> host = {};
            inet_pton(AF_INET6, addr.routing_prefix.c_str(), host.data());
#ifdef CANOPY_FIXED_ADDRESS_SIZE
            uint64_t prefix = 0;
            for (int i = 0; i < 8; ++i)
                prefix = (prefix << 8) | host[i];
            rpc::zone_address result(prefix, static_cast<uint32_t>(addr.subnet), static_cast<uint32_t>(addr.object_id));
#else
            rpc::zone_address result(host, 64);
            result.set_subnet(addr.subnet);
            result.set_object_id(addr.object_id);
#endif
            return result;
        }
    }
}
