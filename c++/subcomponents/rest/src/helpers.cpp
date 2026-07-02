/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/helpers.h>

#include <canopy/http_utils/http.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace canopy::rest
{
    namespace
    {
        constexpr auto header_name
            = [](const streaming::http_client::header& header) -> std::string_view { return header.name; };

        enum class field_name_direction
        {
            idl_to_wire,
            wire_to_idl,
        };

        const json::v1::object* find_member(
            const json::v1::map& values,
            std::string_view name)
        {
            const auto item = values.find(std::string(name));
            if (item == values.end())
                return nullptr;
            return &item->second;
        }

        std::string string_member(
            const json::v1::map& values,
            std::string_view name)
        {
            const auto* value = find_member(values, name);
            if (!value || value->get_type() != json::v1::object::type::string_type)
                return {};
            return value->get<std::string>();
        }

        json::v1::object transform_json_value(
            const json::v1::object& value,
            const json::v1::object* field_map,
            field_name_direction direction)
        {
            if (!field_map || field_map->get_type() != json::v1::object::type::map_type)
                return value;

            const auto& map = field_map->as_map();
            if (const auto* items = find_member(map, "items");
                items && value.get_type() == json::v1::object::type::array_type)
            {
                json::v1::array output;
                for (const auto& item : value.as_array())
                    output.push_back(transform_json_value(item, items, direction));
                return json::v1::object(std::move(output));
            }

            const auto* additional = find_member(map, "additionalProperties");
            if (value.get_type() != json::v1::object::type::map_type)
                return value;

            const auto& input_fields = value.as_map();
            const auto* fields = find_member(map, "fields");
            if (!fields || fields->get_type() != json::v1::object::type::array_type)
            {
                if (!additional)
                    return value;
                json::v1::map output;
                for (const auto& [name, child] : input_fields)
                    output.emplace(name, transform_json_value(child, additional, direction));
                return json::v1::object(std::move(output));
            }

            std::unordered_set<std::string> matched_fields;
            json::v1::map output;
            for (const auto& field : fields->as_array())
            {
                if (field.get_type() != json::v1::object::type::map_type)
                    continue;

                const auto& field_info = field.as_map();
                const auto idl_name = string_member(field_info, "name");
                const auto wire_name = string_member(field_info, "wire_name");
                if (idl_name.empty() || wire_name.empty())
                    continue;

                const auto& source_name = direction == field_name_direction::idl_to_wire ? idl_name : wire_name;
                const auto& target_name = direction == field_name_direction::idl_to_wire ? wire_name : idl_name;
                const auto item = input_fields.find(source_name);
                if (item == input_fields.end())
                    continue;

                matched_fields.insert(source_name);
                output.emplace(target_name, transform_json_value(item->second, find_member(field_info, "map"), direction));
            }

            for (const auto& [name, child] : input_fields)
            {
                if (matched_fields.find(name) != matched_fields.end())
                    continue;
                output.emplace(name, additional ? transform_json_value(child, additional, direction) : child);
            }
            return json::v1::object(std::move(output));
        }

        std::string transform_json_body(
            std::string_view body,
            std::string_view field_map_json,
            field_name_direction direction)
        {
            if (field_map_json.empty())
                return json::v1::dump(json::v1::parse(body.empty() ? "{}" : body));
            const auto field_map = json::v1::parse(field_map_json);
            const auto value = json::v1::parse(body.empty() ? "{}" : body);
            return json::v1::dump(transform_json_value(value, &field_map, direction));
        }
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
        constexpr std::string_view hex_digits = "0123456789ABCDEF";
        std::string output;
        output.reserve(value.size());
        for (const unsigned char ch : value)
        {
            if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            {
                output.push_back(static_cast<char>(ch));
            }
            else
            {
                output.push_back('%');
                output.push_back(hex_digits[(ch >> 4U) & 0x0FU]);
                output.push_back(hex_digits[ch & 0x0FU]);
            }
        }
        return output;
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

    std::string transform_body_to_wire(
        std::string_view body,
        std::string_view field_map_json)
    {
        return transform_json_body(body, field_map_json, field_name_direction::idl_to_wire);
    }

    std::string transform_body_to_idl(
        std::string_view body,
        std::string_view field_map_json)
    {
        return transform_json_body(body, field_map_json, field_name_direction::wire_to_idl);
    }

    std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name)
    {
        return json::v1::dump(required_input(input, name));
    }

    std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name,
        std::initializer_list<response_field_binding> fields)
    {
        if (fields.size() == 0)
            return body_from_rpc_input(input, name);

        const auto& body = required_input(input, name);
        if (body.get_type() != json::v1::object::type::map_type)
            throw std::runtime_error("REST caller body field is not an object");

        const auto& body_fields = body.as_map();
        json::v1::map wire_fields;
        for (const auto& field : fields)
        {
            const auto item = body_fields.find(std::string(field.output_name));
            if (item == body_fields.end())
            {
                if (field.required)
                    throw std::runtime_error("required REST request field is missing");
                continue;
            }
            if (!field.required && is_null(item->second))
                continue;
            wire_fields.emplace(std::string(field.wire_name), item->second);
        }
        return json::v1::dump(json::v1::object(std::move(wire_fields)));
    }

    std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name,
        std::string_view field_map_json)
    {
        return transform_json_body(
            json::v1::dump(required_input(input, name)), field_map_json, field_name_direction::idl_to_wire);
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
        std::string_view output_name,
        std::string_view body,
        std::string_view field_map_json)
    {
        json::v1::map output_values;
        output_values.emplace(
            std::string(output_name),
            json::v1::parse(transform_json_body(body, field_map_json, field_name_direction::wire_to_idl)));
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

    std::string wrap_response_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body,
        std::string_view field_map_json)
    {
        if (fields.size() == 0)
            return "{}";

        const auto transformed
            = json::v1::parse(transform_json_body(body, field_map_json, field_name_direction::wire_to_idl));
        const auto& response_fields = transformed.as_map();

        json::v1::map output_values;
        for (const auto& field : fields)
        {
            const auto item = response_fields.find(std::string(field.output_name));
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

    json::v1::map response_fields_from_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body)
    {
        return json::v1::parse(wrap_response_body(fields, body)).as_map();
    }

    json::v1::map response_fields_from_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body,
        std::string_view field_map_json)
    {
        return json::v1::parse(wrap_response_body(fields, body, field_map_json)).as_map();
    }
} // namespace canopy::rest
