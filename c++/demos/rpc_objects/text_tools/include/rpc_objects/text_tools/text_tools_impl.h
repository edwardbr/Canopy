/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <rpc/module.h>
#include <rpc/rpc.h>
#include <text_tools/text_tools.h>

namespace text_tools::v1
{
    [[nodiscard]] rpc::shared_ptr<i_text_tools> make_text_tools();
    [[nodiscard]] rpc::shared_ptr<i_text_tools> make_text_tools(std::shared_ptr<rpc::service> service);
    [[nodiscard]] rpc::module::object_factory<
        i_text_tools,
        i_text_tools>
    make_text_tools_factory();
}
