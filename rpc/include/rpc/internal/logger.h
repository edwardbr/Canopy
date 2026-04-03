/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>
#include <cstring>

#ifndef RPC_LOGGING_DEFINED

// Include thread-local logging if enabled (host only, not in enclave)
#  if defined(CANOPY_USE_THREAD_LOCAL_LOGGING) && !defined(_IN_ENCLAVE)
#    include <rpc/internal/thread_local_logger.h>
#  endif

// Determine which logging backend to use
#  if defined(CANOPY_USE_THREAD_LOCAL_LOGGING) && !defined(_IN_ENCLAVE)
// Use thread-local circular buffer logging
#    define RPC_LOG_BACKEND(level, message) rpc::thread_local_log(level, message, __FILE__, __LINE__, __FUNCTION__)
#  elif defined(CANOPY_USE_LOGGING)
// Use standard RPC logging
#    ifdef _IN_ENCLAVE
#      include <sgx_error.h>
extern "C"
{
    sgx_status_t rpc_log(
        int level,
        const char* str,
        size_t sz);
}
#    else
extern "C"
{
    void rpc_log(
        int level,
        const char* str,
        size_t sz);
}
#    endif
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

#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#  endif

// Unified logging macros with levels (0=TRACE, 1=DEBUG, 2=INFO, 3=WARNING, 4=ERROR, 5=CRITICAL)
// CANOPY_LOGGING_LEVEL sets the minimum severity: only messages at or above this level are emitted.
#  if defined(CANOPY_USE_LOGGING) || (defined(CANOPY_USE_THREAD_LOCAL_LOGGING) && !defined(_IN_ENCLAVE))

#    ifdef _IN_ENCLAVE
#      include <fmt/format-inl.h>
#    else
#      include <fmt/format.h>
#    endif

#    ifndef CANOPY_LOGGING_LEVEL
#      define CANOPY_LOGGING_LEVEL 2
#    endif

#    if CANOPY_LOGGING_LEVEL <= 0
#      define RPC_TRACE(format_str, ...)                                                                               \
          do                                                                                                           \
          {                                                                                                            \
              auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                                 \
              RPC_LOG_BACKEND(0, formatted);                                                                           \
          } while (0)
#    else
#      define RPC_TRACE(...)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 1
#      define RPC_DEBUG(format_str, ...)                                                                               \
          do                                                                                                           \
          {                                                                                                            \
              auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                                 \
              RPC_LOG_BACKEND(1, formatted);                                                                           \
          } while (0)
#    else
#      define RPC_DEBUG(...)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 2
#      define RPC_INFO(format_str, ...)                                                                                \
          do                                                                                                           \
          {                                                                                                            \
              auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                                 \
              RPC_LOG_BACKEND(2, formatted);                                                                           \
          } while (0)
#    else
#      define RPC_INFO(...)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 3
#      define RPC_WARNING(format_str, ...)                                                                             \
          do                                                                                                           \
          {                                                                                                            \
              auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                                 \
              RPC_LOG_BACKEND(3, formatted);                                                                           \
          } while (0)
#    else
#      define RPC_WARNING(...)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 4
#      ifdef CANOPY_ASSERT_ON_LOGGER_ERROR
#        define RPC_ERROR(format_str, ...)                                                                             \
            do                                                                                                         \
            {                                                                                                          \
                RPC_ASSERT(false);                                                                                     \
                auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                               \
                RPC_LOG_BACKEND(4, formatted);                                                                         \
            } while (0)
#      else
#        define RPC_ERROR(format_str, ...)                                                                             \
            do                                                                                                         \
            {                                                                                                          \
                auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                               \
                RPC_LOG_BACKEND(4, formatted);                                                                         \
            } while (0)
#      endif
#    else
#      define RPC_ERROR(...)
#    endif

#    if CANOPY_LOGGING_LEVEL <= 5
#      ifdef CANOPY_ASSERT_ON_LOGGER_ERROR
#        define RPC_CRITICAL(format_str, ...)                                                                          \
            do                                                                                                         \
            {                                                                                                          \
                RPC_ASSERT(false);                                                                                     \
                auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                               \
                RPC_LOG_BACKEND(5, formatted);                                                                         \
            } while (0)
#      else
#        define RPC_CRITICAL(format_str, ...)                                                                          \
            do                                                                                                         \
            {                                                                                                          \
                auto formatted = fmt::format(format_str, ##__VA_ARGS__);                                               \
                RPC_LOG_BACKEND(5, formatted);                                                                         \
            } while (0)
#      endif
#    else
#      define RPC_CRITICAL(...)
#    endif

#  else
// Disabled logging - all macros are no-ops.
#      define RPC_TRACE(...)
#      define RPC_DEBUG(...)
#      define RPC_INFO(...)
#      define RPC_WARNING(...)
#      define RPC_ERROR(...)
#      define RPC_CRITICAL(...)
#  endif

#  ifdef __clang__
#    pragma clang diagnostic pop
#  endif

#  define RPC_LOGGING_DEFINED
#endif
