/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef DEBUG_DEFAULT_DESTRUCTOR
#define DEFAULT_DESTRUCTOR                                                                                             \
    {                                                                                                                  \
    }
#else
#define DEFAULT_DESTRUCTOR = default;
#endif

// Macros to disable/enable warnings around YAS usage
#ifdef __clang__
#define YAS_WARNINGS_PUSH                                                                                              \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wpedantic\"")                                \
        _Pragma("clang diagnostic ignored \"-Wsign-conversion\"") _Pragma("clang diagnostic ignored \"-Wconversion\"") \
            _Pragma("clang diagnostic ignored \"-Wimplicit-int-conversion\"")                                          \
                _Pragma("clang diagnostic ignored \"-Wunused-value\"")                                                 \
                    _Pragma("clang diagnostic ignored \"-Wimplicit-fallthrough\"")
#define YAS_WARNINGS_POP _Pragma("clang diagnostic pop")
#else
#define YAS_WARNINGS_PUSH                                                                                              \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wpedantic\"")                                    \
        _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"") _Pragma("GCC diagnostic ignored \"-Wconversion\"")     \
            _Pragma("GCC diagnostic ignored \"-Wunused-value\"")                                                       \
                _Pragma("GCC diagnostic ignored \"-Wimplicit-fallthrough\"")
#define YAS_WARNINGS_POP _Pragma("GCC diagnostic pop")
#endif

// Disable pedantic warnings for YAS library headers
YAS_WARNINGS_PUSH

#include <yas/count_streams.hpp>
#include <yas/serialize.hpp>
#include <yas/std_types.hpp>

YAS_WARNINGS_POP
