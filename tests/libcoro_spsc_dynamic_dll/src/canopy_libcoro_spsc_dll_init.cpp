/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <common/tests.h>
#  include <transports/libcoro_spsc_dynamic_dll/dll_transport.h>

namespace rpc::libcoro_spsc_dynamic_dll
{
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> canopy_libcoro_spsc_dll_init(
        const std::string& name,
        std::shared_ptr<rpc::root_service> service,
        std::shared_ptr<streaming::stream> stream)
    {
        CO_RETURN CO_AWAIT create_acceptor<yyy::i_host, yyy::i_example>(
            name,
            std::move(service),
            std::move(stream),
            [](rpc::shared_ptr<yyy::i_host> host,
                std::shared_ptr<rpc::service> svc) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                CO_RETURN rpc::service_connect_result<yyy::i_example>{
                    rpc::error::OK(), rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host))};
            });
    }
} // namespace rpc::libcoro_spsc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
