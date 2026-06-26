/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <json/convert.h>
#include <json/json_dom.h>
#include <streaming/http_client/client.h>

namespace canopy::rest
{
    struct response_field_binding
    {
        std::string_view output_name;
        std::string_view wire_name;
        bool required{true};
    };

    [[nodiscard]] std::string normalise_json(std::string_view text);
    void add_json_headers(
        std::vector<streaming::http_client::header>& headers,
        bool has_body);
    [[nodiscard]] std::string encode_path_segment(std::string_view value);
    [[nodiscard]] std::string append_path_segment(
        std::string_view base_path,
        std::string_view segment);
    [[nodiscard]] std::string join_target(
        std::string_view base_path,
        std::string_view operation_path);
    void append_query_parameter(
        std::string& target,
        std::string_view name,
        std::string_view value);
    [[nodiscard]] std::string required_string_field(
        std::string_view json_text,
        std::string_view field_name);
    [[nodiscard]] json::v1::map rpc_input_map(const std::vector<char>& buffer);
    [[nodiscard]] const json::v1::object& required_input(
        const json::v1::map& input,
        std::string_view name);
    [[nodiscard]] const json::v1::object* optional_input(
        const json::v1::map& input,
        std::string_view name);
    [[nodiscard]] bool is_null(const json::v1::object& value);
    [[nodiscard]] std::string component_string(const json::v1::object& value);
    [[nodiscard]] std::string replace_path_parameter(
        std::string target,
        std::string_view name,
        std::string_view value);
    [[nodiscard]] std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name);
    [[nodiscard]] std::string wrap_response_body(
        std::string_view output_name,
        std::string_view body);
    [[nodiscard]] std::string wrap_response_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body);

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
