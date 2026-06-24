/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "rest_generator.h"

#include "coreclasses.h"
#include "helpers.h"
#include "interface_declaration_generator.h"
#include "writer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace rest_generator
{
    namespace
    {
        struct rest_parameter
        {
            std::string name;
            std::string location;
            std::string wire_name;
            bool required{false};
        };

        struct rest_method
        {
            std::string name;
            std::string http_method;
            std::string path;
            std::string body_param;
            std::string out_param;
            std::vector<rest_parameter> parameters;
        };

        struct rest_interface
        {
            std::string qualified_name;
            std::string host;
            std::string base_path;
            std::map<std::string, rest_method> methods;
        };

        std::vector<std::string> split_tabs(std::string_view line)
        {
            std::vector<std::string> fields;
            while (true)
            {
                const auto tab = line.find('\t');
                fields.emplace_back(line.substr(0, tab));
                if (tab == std::string_view::npos)
                    break;
                line.remove_prefix(tab + 1);
            }
            return fields;
        }

        std::map<
            std::string,
            rest_interface>
        read_metadata(const std::filesystem::path& path)
        {
            std::ifstream input(path);
            if (!input)
                throw std::runtime_error("failed to open REST metadata file: " + path.string());

            std::map<std::string, rest_interface> interfaces;
            std::string line;
            while (std::getline(input, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;

                auto fields = split_tabs(line);
                if (fields.empty())
                    continue;

                if (fields[0] == "interface")
                {
                    if (fields.size() != 4)
                        throw std::runtime_error("invalid REST interface metadata line: " + line);
                    auto& item = interfaces[fields[1]];
                    item.qualified_name = fields[1];
                    item.host = fields[2];
                    item.base_path = fields[3];
                }
                else if (fields[0] == "method")
                {
                    if (fields.size() < 5 || fields.size() > 7)
                        throw std::runtime_error("invalid REST method metadata line: " + line);
                    auto& item = interfaces[fields[1]];
                    item.qualified_name = fields[1];
                    auto& method = item.methods[fields[2]];
                    method.name = fields[2];
                    method.http_method = fields[3];
                    method.path = fields[4];
                    method.body_param = fields.size() > 5 ? fields[5] : "";
                    method.out_param = fields.size() > 6 ? fields[6] : "";
                }
                else if (fields[0] == "param")
                {
                    if (fields.size() != 7)
                        throw std::runtime_error("invalid REST parameter metadata line: " + line);
                    auto& item = interfaces[fields[1]];
                    item.qualified_name = fields[1];
                    auto& method = item.methods[fields[2]];
                    method.name = fields[2];
                    method.parameters.push_back(
                        rest_parameter{
                            fields[3],
                            fields[4],
                            fields[5],
                            fields[6] == "true",
                        });
                }
                else
                {
                    throw std::runtime_error("unknown REST metadata line: " + line);
                }
            }

            return interfaces;
        }

        std::string qualified_name(const class_entity& interface_entity)
        {
            std::string scoped_name;
            interface_declaration_generator::build_scoped_name(&interface_entity, scoped_name);
            if (scoped_name.size() >= 2 && scoped_name.substr(scoped_name.size() - 2) == "::")
                scoped_name.resize(scoped_name.size() - 2);
            return scoped_name;
        }

        const function_entity& find_function(
            const class_entity& interface_entity,
            const std::string& name)
        {
            for (const auto& function : interface_entity.get_functions())
            {
                if (function->get_entity_type() == entity_type::FUNCTION_METHOD && function->get_name() == name)
                    return *function;
            }
            throw std::runtime_error("REST metadata references unknown method: " + name);
        }

        std::string render_parameter_list(
            const class_entity& interface_entity,
            const function_entity& function)
        {
            std::stringstream stream;
            writer output(stream);
            bool has_parameter = false;
            for (const auto& parameter : function.get_parameters())
            {
                if (has_parameter)
                    output.raw(", ");
                has_parameter = true;
                render_parameter(output, interface_entity, parameter);
            }
            return stream.str();
        }

        std::string render_argument_list(const function_entity& function)
        {
            std::string output;
            bool has_parameter = false;
            for (const auto& parameter : function.get_parameters())
            {
                if (!is_in_param(parameter))
                    continue;
                if (has_parameter)
                    output += ", ";
                has_parameter = true;
                output += parameter.get_name();
            }
            return output;
        }

        bool method_is_const(const function_entity& function)
        {
            return function.has_value("const");
        }

        bool has_out_parameters(const function_entity& function)
        {
            return std::any_of(
                function.get_parameters().begin(),
                function.get_parameters().end(),
                [](const auto& parameter) { return is_out_param(parameter); });
        }

        bool has_out_parameter(
            const function_entity& function,
            const std::string& name)
        {
            return std::any_of(
                function.get_parameters().begin(),
                function.get_parameters().end(),
                [&name](const auto& parameter) { return is_out_param(parameter) && parameter.get_name() == name; });
        }

        void write_request_parameter(
            writer& output,
            const rest_parameter& parameter)
        {
            if (parameter.location == "body")
                return;

            const auto parameter_name = cpp_string_literal(parameter.name);
            const auto wire_name = cpp_string_literal(parameter.wire_name);
            if (parameter.required)
            {
                output(
                    "const auto& __rest_{0} = __canopy_rest_required_input(__rest_input, {1});",
                    parameter.name,
                    parameter_name);
            }
            else
            {
                output(
                    "const auto* __rest_{0} = __canopy_rest_optional_input(__rest_input, {1});",
                    parameter.name,
                    parameter_name);
            }

            if (parameter.location == "path")
            {
                output(
                    "__request.target = __canopy_rest_replace_path_parameter(__request.target, {0}, "
                    "__canopy_rest_component(__rest_{1}));",
                    wire_name,
                    parameter.name);
            }
            else if (parameter.location == "query")
            {
                if (parameter.required)
                {
                    output(
                        "canopy::rest::append_query_parameter(__request.target, {0}, "
                        "__canopy_rest_component(__rest_{1}));",
                        wire_name,
                        parameter.name);
                }
                else
                {
                    output("if (__rest_{0} && !__canopy_rest_is_null(*__rest_{0}))", parameter.name);
                    output("{{");
                    output(
                        "canopy::rest::append_query_parameter(__request.target, {0}, "
                        "__canopy_rest_component(*__rest_{1}));",
                        wire_name,
                        parameter.name);
                    output("}}");
                }
            }
        }

        void write_rest_method(
            writer& output,
            const class_entity& interface_entity,
            const rest_method& method)
        {
            const auto& function = find_function(interface_entity, method.name);
            const auto interface_name = interface_entity.get_name();
            const auto return_type = render_cpp_type(interface_entity, function.get_return_type());
            const auto parameter_list = render_parameter_list(interface_entity, function);
            const auto argument_list = render_argument_list(function);
            const auto const_suffix = method_is_const(function) ? " const" : "";
            const auto body_param = cpp_string_literal(method.body_param);
            const auto out_param = cpp_string_literal(method.out_param);
            if (method.out_param.empty())
            {
                if (has_out_parameters(function))
                    throw std::runtime_error(
                        "REST metadata omits response parameter for method with IDL out parameters: " + method.name);
            }
            else if (!has_out_parameter(function, method.out_param))
            {
                throw std::runtime_error("REST metadata references unknown response parameter for method: " + method.name);
            }

            output.print_tabs();
            output.raw(
                "CORO_TASK({}) {}::rest_caller::{}({}){}\n", return_type, interface_name, method.name, parameter_list, const_suffix);
            output("{{");
            output("try");
            output("{{");
            output("std::vector<char> __rpc_in_buf;");
            output(
                "auto __rpc_ret = {0}::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::{1}({2}{3}__rpc_in_buf, "
                "rpc::encoding::yas_json);",
                interface_name,
                method.name,
                argument_list,
                argument_list.empty() ? "" : ", ");
            output("if (rpc::error::is_error(__rpc_ret))");
            output("{{");
            output("CO_RETURN __rpc_ret;");
            output("}}");
            output("const auto __rest_input = __canopy_rest_input_map(__rpc_in_buf);");
            output("streaming::http_client::request __request;");
            output("__request.method = {};", cpp_string_literal(method.http_method));
            output(
                "__request.target = canopy::rest::join_target(settings_.endpoint.base_path, {});",
                cpp_string_literal(method.path));

            for (const auto& parameter : method.parameters)
                write_request_parameter(output, parameter);

            if (!method.body_param.empty())
            {
                output("__request.body = __canopy_rest_body(__rest_input, {});", body_param);
                output("__canopy_rest_add_json_headers(__request.headers, true);");
            }
            else
            {
                output("__canopy_rest_add_json_headers(__request.headers, false);");
            }

            output("const auto __prepare_error = canopy::rest::prepare_request(__request, settings_);");
            output("if (rpc::error::is_error(__prepare_error))");
            output("{{");
            output("CO_RETURN __prepare_error;");
            output("}}");
            output(
                "auto __http = CO_AWAIT streaming::http_client::send_request(stream_, __request, "
                "settings_.receive_timeout, settings_.max_response_bytes);");
            output("if (__http.error_code != rpc::error::OK())");
            output("{{");
            output("CO_RETURN __http.error_code;");
            output("}}");
            output("if (__http.value.status_code < 200 || __http.value.status_code >= 300)");
            output("{{");
            output("CO_RETURN rpc::error::PROTOCOL_ERROR();");
            output("}}");
            if (method.out_param.empty())
            {
                output("CO_RETURN rpc::error::OK();");
            }
            else
            {
                output("auto __wrapped_response = __canopy_rest_wrap_response({}, __http.value.body);", out_param);
                output("std::vector<char> __rpc_out_buf(__wrapped_response.begin(), __wrapped_response.end());");
                output(
                    "__rpc_ret = {0}::proxy_deserialiser<rpc::serialiser::yas, rpc::encoding>::{1}({2}, "
                    "rpc::byte_span(__rpc_out_buf), rpc::encoding::yas_json);",
                    interface_name,
                    method.name,
                    method.out_param);
                output("CO_RETURN __rpc_ret;");
            }
            output("}}");
            output("catch (const std::exception&)");
            output("{{");
            output("CO_RETURN rpc::error::INVALID_DATA();");
            output("}}");
            output("}}");
            output("");
        }

        void write_rest_interface(
            writer& output,
            const class_entity& interface_entity,
            const rest_interface& metadata)
        {
            const auto interface_name = interface_entity.get_name();
            output(
                "{0}::rest_caller::rest_caller(std::shared_ptr<::streaming::stream>&& stream, rest_settings settings)",
                interface_name);
            output("    : stream_(std::move(stream))");
            output("    , settings_(std::move(settings))");
            output("{{");
            output("if (settings_.endpoint.host.empty())");
            output("{{");
            output("settings_.endpoint.host = {};", cpp_string_literal(metadata.host));
            output("}}");
            output("if (settings_.endpoint.base_path.empty())");
            output("{{");
            output("settings_.endpoint.base_path = {};", cpp_string_literal(metadata.base_path));
            output("}}");
            output("}}");
            output("");

            output(
                "CORO_TASK(::canopy::rest::connect_result<{0}>) {0}::rest_caller::connect(rest_settings settings, "
                "std::shared_ptr<rpc::service> service, ::rpc::connection_factory::context factory_context)",
                interface_name);
            output("{{");
            output("if (settings.endpoint.host.empty())");
            output("{{");
            output("settings.endpoint.host = {};", cpp_string_literal(metadata.host));
            output("}}");
            output("if (settings.endpoint.base_path.empty())");
            output("{{");
            output("settings.endpoint.base_path = {};", cpp_string_literal(metadata.base_path));
            output("}}");
            output("auto stream = CO_AWAIT canopy::rest::connect_stream(settings, std::move(service), std::move(factory_context));");
            output("if (stream.error_code != rpc::error::OK() || !stream.stream)");
            output("{{");
            output("CO_RETURN ::canopy::rest::connect_result<{0}>{{stream.error_code, {{}}, {{}}}};", interface_name);
            output("}}");
            output("auto __stream_handle = stream.stream;");
            output(
                "rpc::shared_ptr<{0}> object(new {0}::rest_caller(std::move(stream.stream), std::move(settings)));",
                interface_name);
            output(
                "CO_RETURN ::canopy::rest::connect_result<{0}>{{rpc::error::OK(), std::move(object), "
                "std::move(__stream_handle)}};",
                interface_name);
            output("}}");
            output("");

            for (const auto& [_, method] : metadata.methods)
                write_rest_method(output, interface_entity, method);
        }

        void write_namespace(
            const class_entity& lib,
            writer& output,
            const std::map<
                std::string,
                rest_interface>& metadata)
        {
            for (const auto& elem : lib.get_elements(entity_type::NAMESPACE_MEMBERS))
            {
                if (elem->is_in_import())
                    continue;
                if (elem->get_entity_type() == entity_type::NAMESPACE)
                {
                    const auto is_inline = elem->has_value("inline");
                    output("{}namespace {}", is_inline ? "inline " : "", elem->get_name());
                    output("{{");
                    write_namespace(static_cast<const class_entity&>(*elem), output, metadata);
                    output("}}");
                }
                else if (elem->get_entity_type() == entity_type::INTERFACE)
                {
                    const auto& interface_entity = static_cast<const class_entity&>(*elem);
                    const auto found = metadata.find(qualified_name(interface_entity));
                    if (found != metadata.end())
                        write_rest_interface(output, interface_entity, found->second);
                }
            }
        }
    } // namespace

    void write_files(
        const class_entity& lib,
        std::ostream& output_stream,
        const std::vector<std::string>& namespaces,
        const std::string& header_filename,
        const std::filesystem::path& metadata_path)
    {
        const auto metadata = read_metadata(metadata_path);
        writer output(output_stream);

        output("#include <algorithm>");
        output("#include <chrono>");
        output("#include <cctype>");
        output("#include <memory>");
        output("#include <stdexcept>");
        output("#include <string>");
        output("#include <string_view>");
        output("#include <utility>");
        output("#include <vector>");
        output("");
        output("#include <canopy/rest/helpers.h>");
        output("#include <json/json_dom.h>");
        output("#include <rpc/rpc.h>");
        output("#include <streaming/http_client/client.h>");
        output("#include \"{}\"", header_filename);
        output("");
        output("// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)");
        output("namespace");
        output("{{");
        output("[[maybe_unused]] bool __canopy_rest_header_name_equals(std::string_view lhs, std::string_view rhs)");
        output("{{");
        output("if (lhs.size() != rhs.size())");
        output("    return false;");
        output("for (size_t index = 0; index < lhs.size(); ++index)");
        output("{{");
        output("    const auto lhs_ch = static_cast<unsigned char>(lhs[index]);");
        output("    const auto rhs_ch = static_cast<unsigned char>(rhs[index]);");
        output("    if (std::tolower(lhs_ch) != std::tolower(rhs_ch))");
        output("        return false;");
        output("}}");
        output("return true;");
        output("}}");
        output("");
        output(
            "[[maybe_unused]] bool __canopy_rest_has_header(const std::vector<streaming::http_client::header>& "
            "headers, "
            "std::string_view name)");
        output("{{");
        output(
            "return std::any_of(headers.begin(), headers.end(), [name](const auto& header) {{ return "
            "__canopy_rest_header_name_equals(header.name, name); }});");
        output("}}");
        output("");
        output("[[maybe_unused]] void __canopy_rest_add_json_headers(std::vector<streaming::http_client::header>& headers, bool has_body)");
        output("{{");
        output("if (!__canopy_rest_has_header(headers, \"Accept\"))");
        output("    headers.push_back({{\"Accept\", \"application/json\"}});");
        output("if (has_body && !__canopy_rest_has_header(headers, \"Content-Type\"))");
        output("    headers.push_back({{\"Content-Type\", \"application/json\"}});");
        output("}}");
        output("");
        output("[[maybe_unused]] json::v1::map __canopy_rest_input_map(const std::vector<char>& buffer)");
        output("{{");
        output("const auto envelope = json::v1::parse(std::string_view(buffer.data(), buffer.size()));");
        output("const auto& envelope_map = envelope.as_map();");
        output("const auto in_item = envelope_map.find(\"in\");");
        output("if (in_item != envelope_map.end() && in_item->second.get_type() == json::v1::object::type::map_type)");
        output("{{");
        output("return in_item->second.as_map();");
        output("}}");
        output("return envelope_map;");
        output("}}");
        output("");
        output("[[maybe_unused]] const json::v1::object& __canopy_rest_required_input(const json::v1::map& input, std::string_view name)");
        output("{{");
        output("const auto item = input.find(std::string(name));");
        output("if (item == input.end())");
        output("    throw std::runtime_error(\"REST caller input field is missing\");");
        output("return item->second;");
        output("}}");
        output("");
        output("[[maybe_unused]] const json::v1::object* __canopy_rest_optional_input(const json::v1::map& input, std::string_view name)");
        output("{{");
        output("const auto item = input.find(std::string(name));");
        output("if (item == input.end())");
        output("    return nullptr;");
        output("return &item->second;");
        output("}}");
        output("");
        output("[[maybe_unused]] bool __canopy_rest_is_null(const json::v1::object& value)");
        output("{{");
        output("return value.get_type() == json::v1::object::type::null_type;");
        output("}}");
        output("");
        output("[[maybe_unused]] std::string __canopy_rest_component(const json::v1::object& value)");
        output("{{");
        output("if (value.get_type() == json::v1::object::type::string_type)");
        output("    return value.get<std::string>();");
        output("return json::v1::dump(value);");
        output("}}");
        output("");
        output(
            "[[maybe_unused]] std::string __canopy_rest_replace_path_parameter(std::string target, std::string_view "
            "name, "
            "std::string_view value)");
        output("{{");
        output("const std::string placeholder = \"{{\" + std::string(name) + \"}}\";");
        output("const auto position = target.find(placeholder);");
        output("if (position == std::string::npos)");
        output("    throw std::runtime_error(\"REST caller path parameter placeholder is missing\");");
        output("target.replace(position, placeholder.size(), canopy::rest::encode_path_segment(value));");
        output("return target;");
        output("}}");
        output("");
        output("[[maybe_unused]] std::string __canopy_rest_body(const json::v1::map& input, std::string_view name)");
        output("{{");
        output("return json::v1::dump(__canopy_rest_required_input(input, name));");
        output("}}");
        output("");
        output("[[maybe_unused]] std::string __canopy_rest_wrap_response(std::string_view output_name, std::string_view body)");
        output("{{");
        output("json::v1::map output_values;");
        output("output_values.emplace(std::string(output_name), json::v1::parse(body));");
        output("return json::v1::dump(json::v1::object(std::move(output_values)));");
        output("}}");
        output("}} // namespace");
        output("");

        for (auto& ns : namespaces)
        {
            output("namespace {}", ns);
            output("{{");
        }

        write_namespace(lib, output, metadata);

        for (auto& ns : namespaces)
        {
            (void)ns;
            output("}}");
        }
        output("// NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)");
    }
} // namespace rest_generator
