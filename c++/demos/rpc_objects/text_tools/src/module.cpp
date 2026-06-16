/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc_objects/object_registration.h>
#include <rpc_objects/text_tools/text_tools_impl.h>

static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params)
{
    CO_RETURN CO_AWAIT rpc::register_object<text_tools::v1::i_text_tools, text_tools::v1::i_text_tools>(
        params, text_tools::v1::make_text_tools_factory());
}
