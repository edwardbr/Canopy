/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace canopy::http_utils
{
    [[nodiscard]] std::string ascii_lower_copy(std::string value);
    [[nodiscard]] std::string_view trim_ascii(std::string_view input);
    [[nodiscard]] std::string trim_ascii_copy(std::string_view input);
    [[nodiscard]] bool ascii_iequals(
        std::string_view lhs,
        std::string_view rhs);

    template<
        class Headers,
        class NameOf>
    [[nodiscard]] inline auto find_header(
        Headers& headers,
        std::string_view name,
        NameOf name_of)
    {
        return std::find_if(
            std::begin(headers),
            std::end(headers),
            [name, name_of](const auto& header) { return ascii_iequals(name_of(header), name); });
    }

    template<
        class Headers,
        class NameOf>
    [[nodiscard]] inline auto find_header(
        const Headers& headers,
        std::string_view name,
        NameOf name_of)
    {
        return std::find_if(
            std::begin(headers),
            std::end(headers),
            [name, name_of](const auto& header) { return ascii_iequals(name_of(header), name); });
    }

    template<
        class Headers,
        class NameOf>
    [[nodiscard]] inline bool has_header(
        const Headers& headers,
        std::string_view name,
        NameOf name_of)
    {
        return find_header(headers, name, name_of) != std::end(headers);
    }

    template<
        class Headers,
        class NameOf,
        class ValueOf>
    [[nodiscard]] inline std::optional<std::string_view> find_header_value(
        const Headers& headers,
        std::string_view name,
        NameOf name_of,
        ValueOf value_of)
    {
        const auto item = find_header(headers, name, name_of);
        if (item == std::end(headers))
            return std::nullopt;
        return std::string_view(value_of(*item));
    }

    [[nodiscard]] bool is_http_token_char(unsigned char ch);
    [[nodiscard]] bool is_http_token(std::string_view value);
    [[nodiscard]] bool has_header_value_control(std::string_view value);
    [[nodiscard]] bool is_status_text_safe(std::string_view value);
    [[nodiscard]] bool is_header_safe(
        std::string_view name,
        std::string_view value);
    [[nodiscard]] bool header_contains_token(
        std::string_view value,
        std::string_view token);
    [[nodiscard]] size_t parse_content_length(std::string_view value);
    [[nodiscard]] std::string decode_chunked_body(std::string_view body);
    [[nodiscard]] std::string request_path(
        std::string_view target,
        std::string_view empty_path = "/");
    [[nodiscard]] std::string percent_decode(
        std::string_view value,
        bool plus_is_space = false);
    [[nodiscard]] std::optional<std::string> percent_decode_strict(
        std::string_view value,
        bool plus_is_space = false);
}
