/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <transports/libcoro_spsc_dynamic_dll/dll_transport.h>

namespace rpc::libcoro_spsc_dynamic_dll
{
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> canopy_libcoro_spsc_dll_init(
        const std::string& name,
        std::shared_ptr<rpc::root_service> service,
        std::shared_ptr<streaming::stream> stream)
    {
        CO_RETURN CO_AWAIT create_acceptor<comprehensive::v1::i_data_processor, comprehensive::v1::i_data_processor>(
            name,
            std::move(service),
            std::move(stream),
            [](rpc::shared_ptr<comprehensive::v1::i_data_processor>,
                std::shared_ptr<rpc::service>) -> CORO_TASK(rpc::service_connect_result<comprehensive::v1::i_data_processor>)
            {
                CO_RETURN rpc::service_connect_result<comprehensive::v1::i_data_processor>{
                    rpc::error::OK(), comprehensive::v1::make_benchmark_data_processor()};
            });
    }
}

#endif
