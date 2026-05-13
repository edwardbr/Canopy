/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/polyfill/expected.h>

namespace coro
{
    template<typename value_type, typename error_type> using expected = ::rpc::expected<value_type, error_type>;
    template<typename error_type> using unexpected = ::rpc::unexpected<error_type>;
} // namespace coro
