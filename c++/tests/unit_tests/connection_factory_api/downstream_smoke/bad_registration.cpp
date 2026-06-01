/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/connection_factory.h>

namespace
{
    struct plain_settings
    {
    };

    void register_bad_settings()
    {
        rpc::connection_factory::context context;
        context.register_stream_layer<plain_settings>("bad_settings", {});
    }
} // namespace
