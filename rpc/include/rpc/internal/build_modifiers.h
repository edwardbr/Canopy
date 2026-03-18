/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_DEBUG_DEFAULT_DESTRUCTOR
#define CANOPY_DEFAULT_DESTRUCTOR                                                                                      \
    {                                                                                                                  \
    }
#else
#define CANOPY_DEFAULT_DESTRUCTOR = default;
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

// FLD(x) - designated initializer helper.
// In C++20 coroutine builds expands to .x = enabling proper designated initializer syntax,
// satisfying modernize-use-designated-initializers.
// In C++17 builds expands to nothing, falling back to positional initialization.
//
// Usage: SomeStruct{FLD(field_a) value_a, FLD(field_b) value_b}
// this does not help with readability but it does with safety

#if defined(CANOPY_BUILD_COROUTINE)
#define FLD(x) .x =
#else
#define FLD(x)
#endif
