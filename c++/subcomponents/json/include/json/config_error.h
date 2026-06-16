/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace json
{
    inline namespace v1
    {
        // The exception type thrown by JSON configuration helpers across the
        // subcomponent. Lives in its own header so that lightweight callers
        // (notably <json/convert.h>, which is included by every IDL-generated
        // schema header) do not need to drag in <filesystem>, <fstream>, and
        // the rest of <json/config.h>'s configuration-loading surface.
        class config_error : public std::runtime_error
        {
        public:
            explicit config_error(const std::string& message)
                : std::runtime_error(message)
            {
            }
        };
    } // namespace v1
} // namespace json
