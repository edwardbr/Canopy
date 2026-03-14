#include <websocket_demo/websocket_demo.h>
#include <rpc/rpc.h>

namespace websocket_demo
{
    namespace v1
    {
        object_address to_object_address(const rpc::zone_address& addr);
        rpc::zone_address to_zone_address(const object_address& addr);
    }
}
