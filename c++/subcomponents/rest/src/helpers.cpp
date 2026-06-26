/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/helpers.h>

#include <canopy/http_utils/http.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace canopy::rest
{
    namespace
    {
        constexpr auto header_name
            = [](const streaming::http_client::header& header) -> std::string_view { return header.name; };
    }

    std::string normalise_json(std::string_view text)
    {
        return json::v1::dump(json::v1::parse(text));
    }

    void add_json_headers(
        std::vector<streaming::http_client::header>& headers,
        bool has_body)
    {
        if (!canopy::http_utils::has_header(headers, "Accept", header_name))
            headers.push_back({"Accept", "application/json"});
        if (has_body && !canopy::http_utils::has_header(headers, "Content-Type", header_name))
            headers.push_back({"Content-Type", "application/json"});
    }

    std::string encode_path_segment(std::string_view value)
    {
        std::ostringstream output;
        output << std::uppercase << std::hex;
        for (const unsigned char ch : value)
        {
            if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            {
                output << static_cast<char>(ch);
            }
            else
            {
                output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
            }
        }
        return output.str();
    }

    std::string append_path_segment(
        std::string_view base_path,
        std::string_view segment)
    {
        std::string path(base_path);
        if (path.empty() || path.back() != '/')
            path.push_back('/');
        path += encode_path_segment(segment);
        return path;
    }

    std::string join_target(
        std::string_view base_path,
        std::string_view operation_path)
    {
        std::string target(base_path);
        if (target.empty())
            target = "/";
        if (!operation_path.empty())
        {
            if (target.back() == '/' && operation_path.front() == '/')
                target.pop_back();
            else if (target.back() != '/' && operation_path.front() != '/')
                target.push_back('/');
            target += operation_path;
        }
        return target;
    }

    void append_query_parameter(
        std::string& target,
        std::string_view name,
        std::string_view value)
    {
        target.push_back(target.find('?') == std::string::npos ? '?' : '&');
        target += encode_path_segment(name);
        target.push_back('=');
        target += encode_path_segment(value);
    }

    std::string required_string_field(
        std::string_view json_text,
        std::string_view field_name)
    {
        const auto request = json::v1::parse(json_text);
        const auto& fields = request.as_map();
        const auto item = fields.find(std::string(field_name));
        if (item == fields.end())
            throw std::runtime_error("required REST JSON field is missing: " + std::string(field_name));
        return item->second.get<std::string>();
    }

    json::v1::map rpc_input_map(const std::vector<char>& buffer)
    {
        const auto envelope = json::v1::parse(std::string_view(buffer.data(), buffer.size()));
        const auto& envelope_map = envelope.as_map();
        const auto in_item = envelope_map.find("in");
        if (in_item != envelope_map.end() && in_item->second.get_type() == json::v1::object::type::map_type)
            return in_item->second.as_map();
        return envelope_map;
    }

    const json::v1::object& required_input(
        const json::v1::map& input,
        std::string_view name)
    {
        const auto item = input.find(std::string(name));
        if (item == input.end())
            throw std::runtime_error("REST caller input field is missing");
        return item->second;
    }

    const json::v1::object* optional_input(
        const json::v1::map& input,
        std::string_view name)
    {
        const auto item = input.find(std::string(name));
        if (item == input.end())
            return nullptr;
        return &item->second;
    }

    bool is_null(const json::v1::object& value)
    {
        return value.get_type() == json::v1::object::type::null_type;
    }

    std::string component_string(const json::v1::object& value)
    {
        if (value.get_type() == json::v1::object::type::string_type)
            return value.get<std::string>();
        return json::v1::dump(value);
    }

    std::string replace_path_parameter(
        std::string target,
        std::string_view name,
        std::string_view value)
    {
        const std::string placeholder = "{" + std::string(name) + "}";
        const auto position = target.find(placeholder);
        if (position == std::string::npos)
            throw std::runtime_error("REST caller path parameter placeholder is missing");
        target.replace(position, placeholder.size(), encode_path_segment(value));
        return target;
    }

    std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name)
    {
        return json::v1::dump(required_input(input, name));
    }

    std::string wrap_response_body(
        std::string_view output_name,
        std::string_view body)
    {
        json::v1::map output_values;
        output_values.emplace(std::string(output_name), json::v1::parse(body.empty() ? "{}" : body));
        return json::v1::dump(json::v1::object(std::move(output_values)));
    }

    std::string wrap_response_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body)
    {
        if (fields.size() == 0)
            return "{}";

        const auto response = json::v1::parse(body.empty() ? "{}" : body);
        const auto& response_fields = response.as_map();

        json::v1::map output_values;
        for (const auto& field : fields)
        {
            const auto item = response_fields.find(std::string(field.wire_name));
            if (item == response_fields.end())
            {
                if (field.required)
                    throw std::runtime_error("required REST response field is missing");
                output_values.emplace(std::string(field.output_name), json::v1::object{});
                continue;
            }
            output_values.emplace(std::string(field.output_name), item->second);
        }
        return json::v1::dump(json::v1::object(std::move(output_values)));
    }
} // namespace canopy::rest
