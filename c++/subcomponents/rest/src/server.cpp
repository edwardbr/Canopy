/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/server.h>

#include <canopy/http_utils/http.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace canopy::rest
{
    namespace
    {
        constexpr auto header_name
            = [](const streaming::http_client::header& header) -> std::string_view { return header.name; };
        constexpr auto header_value
            = [](const streaming::http_client::header& header) -> std::string_view { return header.value; };

        [[nodiscard]] std::vector<std::string> split_path(std::string_view path)
        {
            if (path.empty())
                path = "/";

            while (path.size() > 1 && path.back() == '/')
                path.remove_suffix(1);

            if (!path.empty() && path.front() == '/')
                path.remove_prefix(1);

            std::vector<std::string> parts;
            while (!path.empty())
            {
                const auto slash = path.find('/');
                parts.emplace_back(path.substr(0, slash));
                if (slash == std::string_view::npos)
                    break;
                path.remove_prefix(slash + 1);
            }
            return parts;
        }

        [[nodiscard]] bool is_placeholder(std::string_view segment)
        {
            return segment.size() >= 3 && segment.front() == '{' && segment.back() == '}';
        }

        [[nodiscard]] std::string placeholder_name(std::string_view segment)
        {
            segment.remove_prefix(1);
            segment.remove_suffix(1);
            return std::string(segment);
        }

        [[nodiscard]] bool consume_literal(
            std::string_view value,
            size_t& offset,
            std::string_view literal)
        {
            if (offset > value.size())
                return false;
            if (value.size() - offset < literal.size())
                return false;
            if (value.substr(offset, literal.size()) != literal)
                return false;
            offset += literal.size();
            return true;
        }

        [[nodiscard]] bool match_segment_template(
            std::string_view path_segment,
            std::string_view pattern_segment,
            std::map<
                std::string,
                std::string>& parameters)
        {
            const auto decoded_segment = canopy::http_utils::percent_decode(path_segment);
            size_t path_offset = 0;
            size_t pattern_offset = 0;

            while (pattern_offset < pattern_segment.size())
            {
                const auto open = pattern_segment.find('{', pattern_offset);
                if (open == std::string_view::npos)
                    return consume_literal(decoded_segment, path_offset, pattern_segment.substr(pattern_offset))
                           && path_offset == decoded_segment.size();

                if (!consume_literal(
                        decoded_segment, path_offset, pattern_segment.substr(pattern_offset, open - pattern_offset)))
                    return false;

                const auto close = pattern_segment.find('}', open + 1);
                if (close == std::string_view::npos || close == open + 1)
                    return false;

                const auto name = pattern_segment.substr(open + 1, close - open - 1);
                const auto next_open = pattern_segment.find('{', close + 1);
                const auto next_literal_end = next_open == std::string_view::npos ? pattern_segment.size() : next_open;
                const auto next_literal = pattern_segment.substr(close + 1, next_literal_end - close - 1);

                std::string value;
                if (next_literal.empty())
                {
                    if (next_open != std::string_view::npos)
                        return false;
                    value = decoded_segment.substr(path_offset);
                    path_offset = decoded_segment.size();
                }
                else
                {
                    const auto value_end = decoded_segment.find(next_literal, path_offset);
                    if (value_end == std::string::npos)
                        return false;
                    value = decoded_segment.substr(path_offset, value_end - path_offset);
                    path_offset = value_end;
                }

                parameters[std::string(name)] = std::move(value);
                pattern_offset = close + 1;
            }

            return path_offset == decoded_segment.size();
        }

        [[nodiscard]] std::string normalise_registry_base_path(std::string base_path)
        {
            if (base_path.empty())
                return "/";
            if (base_path.front() != '/')
                base_path.insert(base_path.begin(), '/');
            while (base_path.size() > 1 && base_path.back() == '/')
                base_path.pop_back();
            return base_path;
        }

        [[nodiscard]] bool path_has_base_path(
            std::string_view path,
            std::string_view base_path)
        {
            if (base_path.empty() || base_path == "/")
                return true;
            return path == base_path
                   || (path.size() > base_path.size() && path.substr(0, base_path.size()) == base_path
                       && path[base_path.size()] == '/');
        }
    }

    path_match match_path_template(
        std::string_view path,
        std::string_view pattern)
    {
        const auto path_parts = split_path(path);
        const auto pattern_parts = split_path(pattern);
        if (path_parts.size() != pattern_parts.size())
            return {};

        path_match result;
        result.matched = true;
        for (size_t index = 0; index < pattern_parts.size(); ++index)
        {
            const auto& path_segment = path_parts[index];
            const auto& pattern_segment = pattern_parts[index];
            if (is_placeholder(pattern_segment))
            {
                result.parameters.emplace(
                    placeholder_name(pattern_segment), canopy::http_utils::percent_decode(path_segment));
            }
            else if (pattern_segment.find('{') != std::string::npos)
            {
                if (!match_segment_template(path_segment, pattern_segment, result.parameters))
                    return {};
            }
            else if (canopy::http_utils::percent_decode(path_segment) != pattern_segment)
            {
                return {};
            }
        }
        return result;
    }

    std::optional<std::string> first_query_parameter(
        std::string_view target,
        std::string_view name)
    {
        const auto question = target.find('?');
        if (question == std::string_view::npos)
            return std::nullopt;

        auto query = target.substr(question + 1);
        const auto fragment = query.find('#');
        if (fragment != std::string_view::npos)
            query = query.substr(0, fragment);

        while (!query.empty())
        {
            const auto amp = query.find('&');
            const auto pair = query.substr(0, amp);
            const auto equals = pair.find('=');
            const auto key = canopy::http_utils::percent_decode(pair.substr(0, equals), true);
            if (key == name)
            {
                if (equals == std::string_view::npos)
                    return std::string();
                return canopy::http_utils::percent_decode(pair.substr(equals + 1), true);
            }
            if (amp == std::string_view::npos)
                break;
            query.remove_prefix(amp + 1);
        }
        return std::nullopt;
    }

    std::optional<std::string> first_header_value(
        const std::vector<streaming::http_client::header>& headers,
        std::string_view name)
    {
        const auto value = canopy::http_utils::find_header_value(headers, name, header_name, header_value);
        if (!value)
            return std::nullopt;
        return std::string(*value);
    }

    json::v1::object component_to_json(std::string_view value)
    {
        try
        {
            return json::v1::parse(value);
        }
        catch (const std::exception&)
        {
            return json::v1::object(std::string(value));
        }
    }

    std::vector<char> make_rpc_input_buffer(json::v1::map inputs)
    {
        auto text = json::v1::dump(json::v1::object(std::move(inputs)));
        return std::vector<char>(text.begin(), text.end());
    }

    std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::string_view out_param)
    {
        if (out_param.empty())
            return {};
        if (out_buf.empty())
            return "{}";

        const std::string out_text(out_buf);
        const std::string out_key = "\"" + std::string(out_param) + "\":";
        const auto out_key_position = out_text.find(out_key);
        if (out_key_position != std::string::npos)
        {
            const auto value_position = out_key_position + out_key.size();
            if (value_position < out_text.size() && (out_text[value_position] == '}' || out_text[value_position] == ','))
            {
                return "{}";
            }
        }

        const auto output = json::v1::parse(out_text);
        const auto& output_map = output.as_map();
        const auto out_item = output_map.find("out");
        if (out_item != output_map.end() && out_item->second.get_type() != json::v1::object::type::map_type)
            return "{}";
        const auto& values = out_item == output_map.end() ? output_map : out_item->second.as_map();
        const auto value = values.find(std::string(out_param));
        if (value == values.end())
            return "{}";
        return json::v1::dump(value->second);
    }

    std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::string_view out_param,
        std::string_view field_map_json)
    {
        const auto body = response_body_from_rpc_output(out_buf, out_param);
        json::v1::map wrapper;
        wrapper.emplace("__body", json::v1::parse(body.empty() ? "{}" : body));
        return body_from_rpc_input(wrapper, "__body", field_map_json);
    }

    std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::initializer_list<response_field_binding> fields)
    {
        if (fields.size() == 0)
            return {};
        if (out_buf.empty())
            return "{}";

        const std::string out_text(out_buf);
        const auto output = json::v1::parse(out_text);
        const auto& output_map = output.as_map();
        const auto out_item = output_map.find("out");
        if (out_item != output_map.end() && out_item->second.get_type() != json::v1::object::type::map_type)
            return "{}";
        const auto& values = out_item == output_map.end() ? output_map : out_item->second.as_map();

        json::v1::map response_fields;
        for (const auto& field : fields)
        {
            const auto value = values.find(std::string(field.output_name));
            if (value == values.end())
            {
                if (field.required)
                    return "{}";
                continue;
            }
            if (!field.required && is_null(value->second))
                continue;
            response_fields.emplace(std::string(field.wire_name), value->second);
        }
        return json::v1::dump(json::v1::object(std::move(response_fields)));
    }

    std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::initializer_list<response_field_binding> fields,
        std::string_view field_map_json)
    {
        if (fields.size() == 0)
            return {};
        if (out_buf.empty())
            return "{}";

        const std::string out_text(out_buf);
        const auto output = json::v1::parse(out_text);
        const auto& output_map = output.as_map();
        const auto out_item = output_map.find("out");
        if (out_item != output_map.end() && out_item->second.get_type() != json::v1::object::type::map_type)
            return "{}";
        const auto& values = out_item == output_map.end() ? output_map : out_item->second.as_map();

        json::v1::map response_fields;
        for (const auto& field : fields)
        {
            const auto value = values.find(std::string(field.output_name));
            if (value == values.end())
            {
                if (field.required)
                    return "{}";
                continue;
            }
            if (!field.required && is_null(value->second))
                continue;
            response_fields.emplace(std::string(field.output_name), value->second);
        }

        json::v1::map wrapper;
        wrapper.emplace("__body", json::v1::object(std::move(response_fields)));
        return body_from_rpc_input(wrapper, "__body", field_map_json);
    }

    std::string response_body_from_fields(
        json::v1::map output_values,
        std::initializer_list<response_field_binding> fields)
    {
        if (fields.size() == 0)
            return {};

        json::v1::map response_fields;
        for (const auto& field : fields)
        {
            const auto value = output_values.find(std::string(field.output_name));
            if (value == output_values.end())
            {
                if (field.required)
                    return "{}";
                continue;
            }
            if (!field.required && is_null(value->second))
                continue;
            response_fields.emplace(std::string(field.wire_name), value->second);
        }
        return json::v1::dump(json::v1::object(std::move(response_fields)));
    }

    std::string response_body_from_fields(
        json::v1::map output_values,
        std::initializer_list<response_field_binding> fields,
        std::string_view field_map_json)
    {
        if (fields.size() == 0)
            return {};

        json::v1::map response_fields;
        for (const auto& field : fields)
        {
            const auto value = output_values.find(std::string(field.output_name));
            if (value == output_values.end())
            {
                if (field.required)
                    return "{}";
                continue;
            }
            if (!field.required && is_null(value->second))
                continue;
            response_fields.emplace(std::string(field.output_name), value->second);
        }
        return transform_body_to_wire(json::v1::dump(json::v1::object(std::move(response_fields))), field_map_json);
    }

    server_response json_response(
        int status_code,
        std::string body)
    {
        return server_response{status_code, "application/json", std::move(body), {}};
    }

    server_response error_response(
        int status_code,
        std::string message)
    {
        json::v1::map body;
        body.emplace("error", json::v1::object(std::move(message)));
        body.emplace("status", json::v1::object(static_cast<int64_t>(status_code)));
        return json_response(status_code, json::v1::dump(json::v1::object(std::move(body))));
    }

    void append_allowed_method(
        std::string& allowed_methods,
        std::string_view method)
    {
        if (method.empty())
            return;

        size_t offset = 0;
        while (offset < allowed_methods.size())
        {
            const auto comma = allowed_methods.find(',', offset);
            auto token = std::string_view(allowed_methods)
                             .substr(offset, comma == std::string::npos ? std::string::npos : comma - offset);
            while (!token.empty() && token.front() == ' ')
                token.remove_prefix(1);
            while (!token.empty() && token.back() == ' ')
                token.remove_suffix(1);
            if (canopy::http_utils::ascii_iequals(token, method))
                return;
            if (comma == std::string::npos)
                break;
            offset = comma + 1;
        }

        if (!allowed_methods.empty())
            allowed_methods += ", ";
        allowed_methods += method;
    }

    server_response method_not_allowed_response(std::string allowed_methods)
    {
        auto response = error_response(405, "Method not allowed");
        response.headers.push_back({"Allow", std::move(allowed_methods)});
        return response;
    }

    void add_body_input(
        json::v1::map& inputs,
        std::string_view name,
        std::string_view body)
    {
        inputs.emplace(std::string(name), json::v1::parse(body.empty() ? "null" : body));
    }

    void add_body_input(
        json::v1::map& inputs,
        std::string_view name,
        std::string_view body,
        std::initializer_list<response_field_binding> fields)
    {
        if (fields.size() == 0)
        {
            add_body_input(inputs, name, body);
            return;
        }

        const auto request_body = json::v1::parse(body.empty() ? "{}" : body);
        if (request_body.get_type() != json::v1::object::type::map_type)
            throw std::runtime_error("REST request body is not an object");

        const auto& wire_fields = request_body.as_map();
        json::v1::map rpc_fields;
        for (const auto& field : fields)
        {
            const auto item = wire_fields.find(std::string(field.wire_name));
            if (item == wire_fields.end())
            {
                if (field.required)
                    throw std::runtime_error("required REST request field is missing");
                continue;
            }
            rpc_fields.emplace(std::string(field.output_name), item->second);
        }

        inputs.emplace(std::string(name), json::v1::object(std::move(rpc_fields)));
    }

    void add_body_input(
        json::v1::map& inputs,
        std::string_view name,
        std::string_view body,
        std::string_view field_map_json)
    {
        const auto wrapped = json::v1::parse(wrap_response_body("__body", body, field_map_json));
        const auto& wrapped_fields = wrapped.as_map();
        const auto item = wrapped_fields.find("__body");
        if (item == wrapped_fields.end())
            throw std::runtime_error("REST request body mapping failed");
        inputs.emplace(std::string(name), item->second);
    }

    CORO_TASK(rpc::send_result)
    call_rpc_object(
        const rpc::casting_interface& object,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        json::v1::map inputs)
    {
        rpc::send_params params{};
        params.protocol_version = rpc::get_version();
        params.encoding_type = rpc::encoding::yas_json;
        params.interface_id = interface_id;
        params.method_id = method_id;
        params.in_data = make_rpc_input_buffer(std::move(inputs));
        CO_RETURN CO_AWAIT rpc::casting_interface::call(object, std::move(params));
    }

    server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::string_view out_param)
    {
        if (result.error_code != rpc::error::OK())
        {
            if (result.error_code == rpc::error::STUB_DESERIALISATION_ERROR())
                return error_response(400, "Invalid REST request body");
            return error_response(500, "REST RPC call failed");
        }

        if (out_param.empty())
            return server_response{204, "application/json", {}, {}};

        try
        {
            return json_response(
                200,
                response_body_from_rpc_output(std::string_view(result.out_buf.data(), result.out_buf.size()), out_param));
        }
        catch (const std::exception&)
        {
            return error_response(500, "Invalid REST RPC response");
        }
    }

    server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::string_view out_param,
        std::string_view field_map_json)
    {
        if (result.error_code != rpc::error::OK())
        {
            if (result.error_code == rpc::error::STUB_DESERIALISATION_ERROR())
                return error_response(400, "Invalid REST request body");
            return error_response(500, "REST RPC call failed");
        }

        if (out_param.empty())
            return server_response{204, "application/json", {}, {}};

        try
        {
            return json_response(
                200,
                response_body_from_rpc_output(
                    std::string_view(result.out_buf.data(), result.out_buf.size()), out_param, field_map_json));
        }
        catch (const std::exception&)
        {
            return error_response(500, "Invalid REST RPC response");
        }
    }

    server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::initializer_list<response_field_binding> fields)
    {
        if (result.error_code != rpc::error::OK())
        {
            if (result.error_code == rpc::error::STUB_DESERIALISATION_ERROR())
                return error_response(400, "Invalid REST request body");
            return error_response(500, "REST RPC call failed");
        }

        if (fields.size() == 0)
            return server_response{204, "application/json", {}, {}};

        try
        {
            return json_response(
                200, response_body_from_rpc_output(std::string_view(result.out_buf.data(), result.out_buf.size()), fields));
        }
        catch (const std::exception&)
        {
            return error_response(500, "Invalid REST RPC response");
        }
    }

    server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::initializer_list<response_field_binding> fields,
        std::string_view field_map_json)
    {
        if (result.error_code != rpc::error::OK())
        {
            if (result.error_code == rpc::error::STUB_DESERIALISATION_ERROR())
                return error_response(400, "Invalid REST request body");
            return error_response(500, "REST RPC call failed");
        }

        if (fields.size() == 0)
            return server_response{204, "application/json", {}, {}};

        try
        {
            return json_response(
                200,
                response_body_from_rpc_output(
                    std::string_view(result.out_buf.data(), result.out_buf.size()), fields, field_map_json));
        }
        catch (const std::exception&)
        {
            return error_response(500, "Invalid REST RPC response");
        }
    }

    void endpoint_registry::add_endpoint(
        std::string base_path,
        server_handler handler,
        std::string name)
    {
        if (!handler)
            return;
        endpoints_.push_back(
            endpoint{normalise_registry_base_path(std::move(base_path)), std::move(name), std::move(handler)});
    }

    bool endpoint_registry::empty() const noexcept
    {
        return endpoints_.empty();
    }

    bool endpoint_registry::may_handle(std::string_view target) const
    {
        const auto path = canopy::http_utils::request_path(target);
        return std::any_of(
            endpoints_.begin(),
            endpoints_.end(),
            [&path](const endpoint& item) { return path_has_base_path(path, item.base_path); });
    }

    CORO_TASK(std::optional<server_response>)
    endpoint_registry::handle(const server_request& request) const
    {
        for (const auto& endpoint : endpoints_)
        {
            if (!path_has_base_path(canopy::http_utils::request_path(request.target), endpoint.base_path))
                continue;

            auto response = CO_AWAIT endpoint.handler(request);
            if (response)
                CO_RETURN response;
        }
        CO_RETURN std::nullopt;
    }

    namespace detail
    {
        json::v1::object bool_component_to_json(std::string_view value)
        {
            std::string lowered(value);
            std::transform(
                lowered.begin(),
                lowered.end(),
                lowered.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lowered == "true" || lowered == "1")
                return json::v1::object(true);
            if (lowered == "false" || lowered == "0")
                return json::v1::object(false);
            throw std::runtime_error("REST boolean parameter is invalid");
        }

        json::v1::object signed_component_to_json(std::string_view value)
        {
            int64_t parsed = 0;
            const auto* begin = value.data();
            const auto* end = begin + value.size();
            const auto result = std::from_chars(begin, end, parsed, 10);
            if (result.ec != std::errc{} || result.ptr != end)
                throw std::runtime_error("REST signed integer parameter is invalid");
            return json::v1::object(parsed);
        }

        json::v1::object unsigned_component_to_json(std::string_view value)
        {
            uint64_t parsed = 0;
            const auto* begin = value.data();
            const auto* end = begin + value.size();
            const auto result = std::from_chars(begin, end, parsed, 10);
            if (result.ec != std::errc{} || result.ptr != end)
                throw std::runtime_error("REST unsigned integer parameter is invalid");
            return json::v1::object(parsed);
        }

        json::v1::object floating_component_to_json(std::string_view value)
        {
            std::string text(value);
            char* end = nullptr;
            errno = 0;
            const auto parsed = std::strtod(text.c_str(), &end);
            if (errno != 0 || end == text.c_str() || *end != '\0')
                throw std::runtime_error("REST floating point parameter is invalid");
            return json::v1::object(parsed);
        }
    }
}
