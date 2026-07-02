/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>
#include <string_view>
#include <cstring>
#include <utility>

#include <rpc/internal/polyfill/format.h>

#ifndef RPC_LOGGING_DEFINED

// Determine which logging backend to use
#  if defined(CANOPY_USE_LOGGING)

extern "C"
{
    void rpc_log(
        int level,
        const char* str,
        size_t sz);
}
#    define RPC_LOG_BACKEND(level, message) rpc_log(level, (message).c_str(), (message).length())
#  else
// No logging - backend is a no-op
#    define RPC_LOG_BACKEND(level, message)                                                                            \
        do                                                                                                             \
        {                                                                                                              \
            (void)(level);                                                                                             \
            (void)(message);                                                                                           \
        } while (0)
#  endif

namespace rpc::detail
{
    template<typename... Args>
    inline void log_message_noexcept(
        int level,
        std::string_view format_str,
        Args&&... args) noexcept
    {
        try
        {
            auto formatted = rpc::format(format_str, std::forward<Args>(args)...);
            RPC_LOG_BACKEND(level, formatted);
        }
        catch (...)
        {
            // Logging must not affect control flow, especially from noexcept cleanup paths.
            return;
        }
    }
} // namespace rpc::detail

#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#  elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wdangling-reference"
#  endif

#  define RPC_LOG_NOOP(...)                                                                                            \
      do                                                                                                               \
      {                                                                                                                \
          (void)0;                                                                                                     \
      } while (0)

// Unified logging macros with levels (0=TRACE, 1=DEBUG, 2=INFO, 3=WARNING, 4=ERROR, 5=CRITICAL)
// CANOPY_LOGGING_LEVEL sets the minimum severity: only messages at or above this level are emitted.
#  if defined(CANOPY_USE_LOGGING)

#    ifndef CANOPY_LOGGING_LEVEL
#      define CANOPY_LOGGING_LEVEL 2
#    endif

#    if CANOPY_LOGGING_LEVEL <= 0
#      define RPC_TRACE(format_str, ...)                                                                               \
          do                                                                                                           \
          {                                                                                                            \
              rpc::detail::log_message_noexcept(0, format_str, ##__VA_ARGS__);                                         \
          } while (0)
#    else
#      define RPC_TRACE(...) RPC_LOG_NOOP(__VA_ARGS__)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 1
#      define RPC_DEBUG(format_str, ...)                                                                               \
          do                                                                                                           \
          {                                                                                                            \
              rpc::detail::log_message_noexcept(1, format_str, ##__VA_ARGS__);                                         \
          } while (0)
#    else
#      define RPC_DEBUG(...) RPC_LOG_NOOP(__VA_ARGS__)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 2
#      define RPC_INFO(format_str, ...)                                                                                \
          do                                                                                                           \
          {                                                                                                            \
              rpc::detail::log_message_noexcept(2, format_str, ##__VA_ARGS__);                                         \
          } while (0)
#    else
#      define RPC_INFO(...) RPC_LOG_NOOP(__VA_ARGS__)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 3
#      define RPC_WARNING(format_str, ...)                                                                             \
          do                                                                                                           \
          {                                                                                                            \
              rpc::detail::log_message_noexcept(3, format_str, ##__VA_ARGS__);                                         \
          } while (0)
#    else
#      define RPC_WARNING(...) RPC_LOG_NOOP(__VA_ARGS__)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 4
#      ifdef CANOPY_ASSERT_ON_LOGGER_ERROR
#        define RPC_ERROR(format_str, ...)                                                                             \
            do                                                                                                         \
            {                                                                                                          \
                RPC_ASSERT(false);                                                                                     \
                rpc::detail::log_message_noexcept(4, format_str, ##__VA_ARGS__);                                       \
            } while (0)
#      else
#        define RPC_ERROR(format_str, ...)                                                                             \
            do                                                                                                         \
            {                                                                                                          \
                rpc::detail::log_message_noexcept(4, format_str, ##__VA_ARGS__);                                       \
            } while (0)
#      endif
#    else
#      define RPC_ERROR(...) RPC_LOG_NOOP(__VA_ARGS__)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 5
#      ifdef CANOPY_ASSERT_ON_LOGGER_ERROR
#        define RPC_CRITICAL(format_str, ...)                                                                          \
            do                                                                                                         \
            {                                                                                                          \
                RPC_ASSERT(false);                                                                                     \
                rpc::detail::log_message_noexcept(5, format_str, ##__VA_ARGS__);                                       \
            } while (0)
#      else
#        define RPC_CRITICAL(format_str, ...)                                                                          \
            do                                                                                                         \
            {                                                                                                          \
                rpc::detail::log_message_noexcept(5, format_str, ##__VA_ARGS__);                                       \
            } while (0)
#      endif
#    else
#      define RPC_CRITICAL(...) RPC_LOG_NOOP(__VA_ARGS__)
#    endif

#  else
// Disabled logging - all macros are no-ops.
#    define RPC_TRACE(...) RPC_LOG_NOOP(__VA_ARGS__)
#    define RPC_DEBUG(...) RPC_LOG_NOOP(__VA_ARGS__)
#    define RPC_INFO(...) RPC_LOG_NOOP(__VA_ARGS__)
#    define RPC_WARNING(...) RPC_LOG_NOOP(__VA_ARGS__)
#    define RPC_ERROR(...) RPC_LOG_NOOP(__VA_ARGS__)
#    define RPC_CRITICAL(...) RPC_LOG_NOOP(__VA_ARGS__)
#  endif

#  if defined(__clang__)
#    pragma clang diagnostic pop
#  elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#  endif

#  define RPC_LOGGING_DEFINED
#endif
