/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#if defined(__has_include)
#  if __has_include(<format>) && !defined(FOR_SGX) && ((defined(_MSVC_LANG) && _MSVC_LANG >= 202002L) || __cplusplus >= 202002L)
#    include <format>
#    if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#      define RPC_HAS_STD_FORMAT 1
#    endif
#  endif
#endif

#ifndef RPC_HAS_STD_FORMAT
#  ifdef FOR_SGX
#    include <fmt/format-inl.h>
#  else
#    include <fmt/format.h>
#  endif
#endif

namespace rpc
{
#ifdef RPC_HAS_STD_FORMAT
    template<typename... Args>
    inline std::string format(
        std::string_view format_string,
        Args&&... args)
    {
        return std::vformat(format_string, std::make_format_args(std::forward<Args>(args)...));
    }

    template<typename OutputIt, typename... Args>
    inline OutputIt format_to(
        OutputIt out,
        std::string_view format_string,
        Args&&... args)
    {
        const auto formatted = rpc::format(format_string, std::forward<Args>(args)...);
        return std::copy(formatted.begin(), formatted.end(), out);
    }
#else
    template<typename... Args>
    inline std::string format(
        std::string_view format_string,
        Args&&... args)
    {
        return fmt::vformat(
            fmt::string_view(format_string.data(), format_string.size()),
            fmt::make_format_args(std::forward<Args>(args)...));
    }

    template<typename OutputIt, typename... Args>
    inline OutputIt format_to(
        OutputIt out,
        std::string_view format_string,
        Args&&... args)
    {
        const auto formatted = rpc::format(format_string, std::forward<Args>(args)...);
        return std::copy(formatted.begin(), formatted.end(), out);
    }
#endif
}
