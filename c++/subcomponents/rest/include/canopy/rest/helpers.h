/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>
#include <string_view>

#include <json/convert.h>
#include <json/json_dom.h>

namespace canopy::rest
{
    [[nodiscard]] std::string normalise_json(std::string_view text);
    [[nodiscard]] std::string encode_path_segment(std::string_view value);
    [[nodiscard]] std::string append_path_segment(
        std::string_view base_path,
        std::string_view segment);
    [[nodiscard]] std::string required_string_field(
        std::string_view json_text,
        std::string_view field_name);

    template<typename T> [[nodiscard]] std::string to_json_string(const T& value)
    {
        using ::json::v1::convert::to_json_object;
        return ::json::v1::dump(to_json_object(value));
    }

    template<typename T> [[nodiscard]] T from_json_string(std::string_view text)
    {
        return ::json::v1::convert::from_json_object<T>(::json::v1::parse(text));
    }
} // namespace canopy::rest
