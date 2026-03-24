/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/libcoro_spsc_dynamic_dll/transport.h>

#  include <streaming/spsc_queue/stream.h>

namespace rpc::libcoro_spsc_dynamic_dll
{
    std::shared_ptr<rpc::stream_transport::transport> make_client(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        queue_pair* queues)
    {
        auto stream = std::make_shared<streaming::spsc_queue::stream>(
            &queues->host_to_dll, &queues->dll_to_host, service->get_scheduler());
        return rpc::stream_transport::make_client(std::move(name), service, std::move(stream));
    }

} // namespace rpc::libcoro_spsc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
