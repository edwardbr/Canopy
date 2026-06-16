/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/connection_factory.h>

#include <memory>
#include <string>

namespace
{
    CORO_TASK(rpc::connection_factory::stream_result)
    make_empty_stream(
        rpc::connection_factory::service_settings,
        std::shared_ptr<rpc::service>,
        const rpc::connection_factory::context&)
    {
        CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), {}};
    }
} // namespace

int main()
{
    const auto materialised
        = rpc::connection_factory::materialise_connection_settings(rpc::connection_factory::empty_options());
    if (materialised.error_code != rpc::error::OK())
        return 1;

    rpc::connection_factory::context context;
    context.set_dependency_value(std::string("downstream-dependency"), "named");

    const auto dependency = context.get_dependency<std::string>("named");
    if (!dependency || *dependency != "downstream-dependency")
        return 2;

    context.register_connect_base_stream<rpc::connection_factory::service_settings>("downstream_smoke", make_empty_stream);

    return 0;
}
