/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <calculator/calculator.h>
#include <rpc/module.h>
#include <rpc/rpc.h>

namespace calculator::v1
{
    [[nodiscard]] rpc::shared_ptr<i_calculator> make_calculator();
    [[nodiscard]] rpc::shared_ptr<i_calculator> make_calculator(std::shared_ptr<rpc::service> service);
    [[nodiscard]] rpc::module::object_factory<
        i_calculator,
        i_calculator>
    make_calculator_factory();
}
