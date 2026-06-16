/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc_objects/calculator/calculator_impl.h>
#include <rpc_objects/object_registration.h>

static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params)
{
    CO_RETURN CO_AWAIT rpc::register_object<calculator::v1::i_calculator, calculator::v1::i_calculator>(
        params, calculator::v1::make_calculator_factory());
}
