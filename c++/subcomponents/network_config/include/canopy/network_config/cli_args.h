/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string>

#include <canopy/network_config/types.h>

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wshadow"
#  pragma clang diagnostic ignored "-Winconsistent-missing-override"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wsuggest-override"
#endif

#include <args.hxx>

#ifdef __clang__
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace canopy::network_config
{
    // Holds the args flag handles registered into an ArgumentParser.
    // Must remain alive until get_config() is called (i.e. after ParseCLI()).
    class network_args_context
    {
        args::Group virtual_addresses_group_;
        mutable args::ValueFlagList<std::string> va_name_args_;
        mutable args::ValueFlagList<std::string> va_type_args_;
        mutable args::ValueFlagList<std::string> va_prefix_args_;
        mutable args::ValueFlagList<uint32_t> va_subnet_bits_args_;
        mutable args::ValueFlagList<uint64_t> va_subnet_args_;
        mutable args::ValueFlagList<uint32_t> va_object_id_bits_args_;
        mutable args::ValueFlagList<uint64_t> va_object_id_args_;

        args::Group endpoints_group_;
        mutable args::ValueFlagList<std::string> listen_args_;
        mutable args::ValueFlagList<std::string> connect_args_;

    public:
        explicit network_args_context(args::ArgumentParser& parser);

        // Extract and validate a network_config after ParseCLI().
        // Endpoints without a name default to the first virtual address.
        // Throws std::invalid_argument on invalid or inconsistent arguments.
        [[nodiscard]] network_config get_config() const;
    };

    // Register network args into parser and return a context that must be kept
    // alive until get_config() is called.
    [[nodiscard]] network_args_context add_network_args(args::ArgumentParser& parser);

    // Extract and validate a network_config from a context after ParseCLI().
    network_config get_network_config(const network_args_context& ctx);

    // Convenience: register args, parse, and return config in one step.
    network_config parse_network_args(
        int argc,
        char** argv,
        args::ArgumentParser& parser);
} // namespace canopy::network_config
