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
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

        struct rest_constant
        {
            std::string location;
            std::string wire_name;
            std::string value;
        };

        struct rest_response_field
        {
            std::string name;
            std::string wire_name;
            bool required{true};
        };

        struct rest_method
        {
            std::string name;
            std::string http_method;
            std::string path;
            std::string body_param;
            std::string body_field_map;
            std::string out_param;
            std::string response_field_map;
            std::string response_content_type;
            int response_status_code{0};
            std::vector<rest_parameter> parameters;
            std::vector<rest_constant> constants;
            std::vector<rest_response_field> request_fields;
            std::vector<rest_response_field> response_fields;
        };

        struct rest_interface
        {
            std::string qualified_name;
            std::string host;
            std::string base_path;
            std::map<std::string, rest_method> methods;
        };

        struct binding_json_value
        {
            enum class type
            {
                null_value,
                bool_value,
                number_value,
                string_value,
                array_value,
                object_value,
            };

            using array = std::vector<binding_json_value>;
            using object = std::map<std::string, binding_json_value>;

            type kind{type::null_value};
            bool boolean{false};
            std::string number;
            std::string string;
            array array_items;
            object object_items;

            static binding_json_value make_null() { return {}; }

            static binding_json_value make_bool(bool value)
            {
                binding_json_value output;
                output.kind = type::bool_value;
                output.boolean = value;
                return output;
            }

            static binding_json_value make_number(std::string value)
            {
                binding_json_value output;
                output.kind = type::number_value;
                output.number = std::move(value);
                return output;
            }

            static binding_json_value make_string(std::string value)
            {
                binding_json_value output;
                output.kind = type::string_value;
                output.string = std::move(value);
                return output;
            }

            static binding_json_value make_array(array value)
            {
                binding_json_value output;
                output.kind = type::array_value;
                output.array_items = std::move(value);
                return output;
            }

            static binding_json_value make_object(object value)
            {
                binding_json_value output;
                output.kind = type::object_value;
                output.object_items = std::move(value);
                return output;
            }
        };

        class binding_json_parser
        {
        public:
            explicit binding_json_parser(std::string_view input)
                : input_(input)
            {
            }

            binding_json_value parse()
            {
                auto value = parse_value();
                skip_whitespace();
                if (!at_end())
                    fail("unexpected trailing data");
                return value;
            }

        private:
            bool at_end() const { return offset_ >= input_.size(); }

            char peek() const
            {
                if (at_end())
                    fail("unexpected end of input");
                return input_[offset_];
            }

            char take()
            {
                const auto value = peek();
                ++offset_;
                return value;
            }

            void skip_whitespace()
            {
                while (!at_end() && std::isspace(static_cast<unsigned char>(input_[offset_])))
                    ++offset_;
            }

            bool consume(char expected)
            {
                skip_whitespace();
                if (at_end() || input_[offset_] != expected)
                    return false;
                ++offset_;
                return true;
            }

            void expect(char expected)
            {
                if (!consume(expected))
                    fail(std::string("expected '") + expected + "'");
            }

            bool consume_literal(std::string_view literal)
            {
                skip_whitespace();
                if (input_.substr(offset_, literal.size()) != literal)
                    return false;
                offset_ += literal.size();
                return true;
            }

            binding_json_value parse_value()
            {
                skip_whitespace();
                if (at_end())
                    fail("expected value");

                const auto value = peek();
                if (value == '{')
                    return parse_object();
                if (value == '[')
                    return parse_array();
                if (value == '"')
                    return binding_json_value::make_string(parse_string());
                if (consume_literal("true"))
                    return binding_json_value::make_bool(true);
                if (consume_literal("false"))
                    return binding_json_value::make_bool(false);
                if (consume_literal("null"))
                    return binding_json_value::make_null();
                if (value == '-' || std::isdigit(static_cast<unsigned char>(value)))
                    return binding_json_value::make_number(parse_number());
                fail("expected object, array, string, boolean, or null");
            }

            std::string parse_number()
            {
                skip_whitespace();
                const auto start = offset_;
                if (!at_end() && input_[offset_] == '-')
                    ++offset_;
                if (at_end())
                    fail("incomplete number");
                if (input_[offset_] == '0')
                {
                    ++offset_;
                }
                else if (std::isdigit(static_cast<unsigned char>(input_[offset_])))
                {
                    while (!at_end() && std::isdigit(static_cast<unsigned char>(input_[offset_])))
                        ++offset_;
                }
                else
                {
                    fail("invalid number");
                }
                if (!at_end() && input_[offset_] == '.')
                {
                    ++offset_;
                    if (at_end() || !std::isdigit(static_cast<unsigned char>(input_[offset_])))
                        fail("invalid fractional number");
                    while (!at_end() && std::isdigit(static_cast<unsigned char>(input_[offset_])))
                        ++offset_;
                }
                if (!at_end() && (input_[offset_] == 'e' || input_[offset_] == 'E'))
                {
                    ++offset_;
                    if (!at_end() && (input_[offset_] == '+' || input_[offset_] == '-'))
                        ++offset_;
                    if (at_end() || !std::isdigit(static_cast<unsigned char>(input_[offset_])))
                        fail("invalid exponent");
                    while (!at_end() && std::isdigit(static_cast<unsigned char>(input_[offset_])))
                        ++offset_;
                }
                return std::string(input_.substr(start, offset_ - start));
            }

            binding_json_value parse_object()
            {
                expect('{');
                binding_json_value::object object;
                if (consume('}'))
                    return binding_json_value::make_object(std::move(object));

                while (true)
                {
                    skip_whitespace();
                    if (peek() != '"')
                        fail("expected object member name");
                    auto key = parse_string();
                    expect(':');
                    const auto result = object.emplace(std::move(key), parse_value());
                    if (!result.second)
                        fail("duplicate object member");
                    if (consume('}'))
                        break;
                    expect(',');
                }
                return binding_json_value::make_object(std::move(object));
            }

            binding_json_value parse_array()
            {
                expect('[');
                binding_json_value::array array;
                if (consume(']'))
                    return binding_json_value::make_array(std::move(array));

                while (true)
                {
                    array.push_back(parse_value());
                    if (consume(']'))
                        break;
                    expect(',');
                }
                return binding_json_value::make_array(std::move(array));
            }

            std::string parse_string()
            {
                expect('"');
                std::string output;
                while (true)
                {
                    if (at_end())
                        fail("unterminated string");
                    const auto value = take();
                    if (value == '"')
                        return output;
                    if (static_cast<unsigned char>(value) < 0x20)
                        fail("control character in string");
                    if (value != '\\')
                    {
                        output.push_back(value);
                        continue;
                    }

                    if (at_end())
                        fail("unterminated escape sequence");
                    const auto escaped = take();
                    switch (escaped)
                    {
                    case '"':
                    case '\\':
                    case '/':
                        output.push_back(escaped);
                        break;
                    case 'b':
                        output.push_back('\b');
                        break;
                    case 'f':
                        output.push_back('\f');
                        break;
                    case 'n':
                        output.push_back('\n');
                        break;
                    case 'r':
                        output.push_back('\r');
                        break;
                    case 't':
                        output.push_back('\t');
                        break;
                    case 'u':
                        append_unicode_escape(output);
                        break;
                    default:
                        fail("invalid escape sequence");
                    }
                }
            }

            static bool is_high_surrogate(uint32_t value) { return value >= 0xD800 && value <= 0xDBFF; }

            static bool is_low_surrogate(uint32_t value) { return value >= 0xDC00 && value <= 0xDFFF; }

            uint32_t parse_hex4()
            {
                if (offset_ + 4 > input_.size())
                    fail("incomplete unicode escape");
                uint32_t value = 0;
                for (size_t index = 0; index < 4; ++index)
                {
                    const auto ch = input_[offset_++];
                    value <<= 4;
                    if (ch >= '0' && ch <= '9')
                        value += static_cast<uint32_t>(ch - '0');
                    else if (ch >= 'a' && ch <= 'f')
                        value += static_cast<uint32_t>(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F')
                        value += static_cast<uint32_t>(ch - 'A' + 10);
                    else
                        fail("invalid unicode escape");
                }
                return value;
            }

            void append_unicode_escape(std::string& output)
            {
                auto codepoint = parse_hex4();
                if (is_high_surrogate(codepoint))
                {
                    if (offset_ + 2 > input_.size() || input_[offset_] != '\\' || input_[offset_ + 1] != 'u')
                        fail("unpaired high surrogate");
                    offset_ += 2;
                    const auto low = parse_hex4();
                    if (!is_low_surrogate(low))
                        fail("invalid low surrogate");
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (is_low_surrogate(codepoint))
                {
                    fail("unpaired low surrogate");
                }
                append_utf8(output, codepoint);
            }

            void append_utf8(
                std::string& output,
                uint32_t codepoint)
            {
                if (codepoint <= 0x7F)
                {
                    output.push_back(static_cast<char>(codepoint));
                }
                else if (codepoint <= 0x7FF)
                {
                    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else if (codepoint <= 0xFFFF)
                {
                    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else if (codepoint <= 0x10FFFF)
                {
                    output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else
                {
                    fail("unicode codepoint out of range");
                }
            }

            [[noreturn]] void fail(const std::string& message) const
            {
                throw std::runtime_error("invalid REST binding JSON at byte " + std::to_string(offset_) + ": " + message);
            }

            std::string_view input_;
            size_t offset_{0};
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

        bool is_path_placeholder(std::string_view segment)
        {
            return segment.size() >= 3 && segment.front() == '{' && segment.back() == '}';
        }

        std::vector<std::string> split_rest_path(std::string_view path)
        {
            while (!path.empty() && path.front() == '/')
                path.remove_prefix(1);
            while (path.size() > 1 && path.back() == '/')
                path.remove_suffix(1);

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

        bool rest_handler_method_less(
            const rest_method* lhs,
            const rest_method* rhs)
        {
            const auto lhs_parts = split_rest_path(lhs->path);
            const auto rhs_parts = split_rest_path(rhs->path);
            const auto count = std::min(lhs_parts.size(), rhs_parts.size());
            for (size_t index = 0; index < count; ++index)
            {
                const auto lhs_placeholder = is_path_placeholder(lhs_parts[index]);
                const auto rhs_placeholder = is_path_placeholder(rhs_parts[index]);
                if (lhs_placeholder != rhs_placeholder)
                    return !lhs_placeholder;
            }

            if (lhs_parts.size() != rhs_parts.size())
                return lhs_parts.size() > rhs_parts.size();
            if (lhs->constants.size() != rhs->constants.size())
                return lhs->constants.size() > rhs->constants.size();
            return lhs->name < rhs->name;
        }

        std::vector<const rest_method*> ordered_rest_handler_methods(const rest_interface& metadata)
        {
            std::vector<const rest_method*> methods;
            methods.reserve(metadata.methods.size());
            for (const auto& [_, method] : metadata.methods)
                methods.push_back(&method);
            std::sort(methods.begin(), methods.end(), rest_handler_method_less);
            return methods;
        }

        std::string read_text_file(const std::filesystem::path& path)
        {
            std::ifstream input(path);
            if (!input)
                throw std::runtime_error("failed to open REST metadata file: " + path.string());
            std::stringstream stream;
            stream << input.rdbuf();
            return stream.str();
        }

        const binding_json_value* find_json_member(
            const binding_json_value::object& object,
            const std::string& key)
        {
            const auto item = object.find(key);
            if (item == object.end())
                return nullptr;
            return &item->second;
        }

        const binding_json_value::object& require_json_map(
            const binding_json_value& value,
            const std::string& path)
        {
            if (value.kind != binding_json_value::type::object_value)
                throw std::runtime_error("REST binding JSON field is not an object: " + path);
            return value.object_items;
        }

        const binding_json_value::array& require_json_array(
            const binding_json_value& value,
            const std::string& path)
        {
            if (value.kind != binding_json_value::type::array_value)
                throw std::runtime_error("REST binding JSON field is not an array: " + path);
            return value.array_items;
        }

        std::string require_json_string(
            const binding_json_value::object& object,
            const std::string& key,
            const std::string& path)
        {
            const auto* value = find_json_member(object, key);
            if (!value)
                throw std::runtime_error("REST binding JSON is missing required field: " + path + "." + key);
            if (value->kind != binding_json_value::type::string_value)
                throw std::runtime_error("REST binding JSON field is not a string: " + path + "." + key);
            return value->string;
        }

        std::string optional_json_string(
            const binding_json_value::object& object,
            const std::string& key,
            const std::string& path)
        {
            const auto* value = find_json_member(object, key);
            if (!value || value->kind == binding_json_value::type::null_value)
                return {};
            if (value->kind != binding_json_value::type::string_value)
                throw std::runtime_error("REST binding JSON field is not a string: " + path + "." + key);
            return value->string;
        }

        bool optional_json_bool(
            const binding_json_value::object& object,
            const std::string& key,
            const std::string& path,
            bool default_value = false)
        {
            const auto* value = find_json_member(object, key);
            if (!value || value->kind == binding_json_value::type::null_value)
                return default_value;
            if (value->kind != binding_json_value::type::bool_value)
                throw std::runtime_error("REST binding JSON field is not a boolean: " + path + "." + key);
            return value->boolean;
        }

        int optional_json_int(
            const binding_json_value::object& object,
            const std::string& key,
            const std::string& path,
            int default_value = 0)
        {
            const auto* value = find_json_member(object, key);
            if (!value || value->kind == binding_json_value::type::null_value)
                return default_value;
            if (value->kind != binding_json_value::type::number_value)
                throw std::runtime_error("REST binding JSON field is not a number: " + path + "." + key);
            const auto& text = value->number;
            if (text.empty() || text.find_first_of(".eE") != std::string::npos)
                throw std::runtime_error("REST binding JSON field is not an integer: " + path + "." + key);
            size_t parsed = 0;
            const auto output = std::stoi(text, &parsed);
            if (parsed != text.size())
                throw std::runtime_error("REST binding JSON field is not an integer: " + path + "." + key);
            return output;
        }

        std::map<
            std::string,
            rest_interface>
        read_tab_metadata(const std::filesystem::path& path)
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
                else if (fields[0] == "constant")
                {
                    if (fields.size() != 6)
                        throw std::runtime_error("invalid REST constant metadata line: " + line);
                    auto& item = interfaces[fields[1]];
                    item.qualified_name = fields[1];
                    auto& method = item.methods[fields[2]];
                    method.name = fields[2];
                    method.constants.push_back(
                        rest_constant{
                            fields[3],
                            fields[4],
                            fields[5],
                        });
                }
                else
                {
                    throw std::runtime_error("unknown REST metadata line: " + line);
                }
            }

            return interfaces;
        }

        std::vector<rest_parameter> read_json_parameters(
            const binding_json_value::object& method_map,
            const std::string& path)
        {
            std::vector<rest_parameter> parameters;
            const auto* parameters_value = find_json_member(method_map, "parameters");
            if (!parameters_value)
                return parameters;
            const auto& parameter_array = require_json_array(*parameters_value, path + ".parameters");
            for (size_t index = 0; index < parameter_array.size(); ++index)
            {
                const auto item_path = path + ".parameters[" + std::to_string(index) + "]";
                const auto& parameter_map = require_json_map(parameter_array[index], item_path);
                parameters.push_back(
                    rest_parameter{
                        require_json_string(parameter_map, "name", item_path),
                        require_json_string(parameter_map, "location", item_path),
                        require_json_string(parameter_map, "wire_name", item_path),
                        optional_json_bool(parameter_map, "required", item_path),
                    });
            }
            return parameters;
        }

        std::vector<rest_constant> read_json_constants(
            const binding_json_value::object& method_map,
            const std::string& path)
        {
            std::vector<rest_constant> constants;
            const auto* constants_value = find_json_member(method_map, "constants");
            if (!constants_value)
                return constants;
            const auto& constant_array = require_json_array(*constants_value, path + ".constants");
            for (size_t index = 0; index < constant_array.size(); ++index)
            {
                const auto item_path = path + ".constants[" + std::to_string(index) + "]";
                const auto& constant_map = require_json_map(constant_array[index], item_path);
                constants.push_back(
                    rest_constant{
                        require_json_string(constant_map, "location", item_path),
                        require_json_string(constant_map, "wire_name", item_path),
                        require_json_string(constant_map, "value", item_path),
                    });
            }
            return constants;
        }

        std::vector<rest_response_field> read_json_response_fields(
            const binding_json_value::object& response_map,
            const std::string& path)
        {
            std::vector<rest_response_field> fields;
            const auto* fields_value = find_json_member(response_map, "fields");
            if (!fields_value)
                return fields;
            const auto& field_array = require_json_array(*fields_value, path + ".fields");
            for (size_t index = 0; index < field_array.size(); ++index)
            {
                const auto item_path = path + ".fields[" + std::to_string(index) + "]";
                const auto& field_map = require_json_map(field_array[index], item_path);
                fields.push_back(
                    rest_response_field{
                        require_json_string(field_map, "name", item_path),
                        require_json_string(field_map, "wire_name", item_path),
                        optional_json_bool(field_map, "required", item_path, true),
                    });
            }
            return fields;
        }

        std::map<
            std::string,
            rest_interface>
        read_json_metadata(const std::filesystem::path& path)
        {
            const auto contents = read_text_file(path);
            const auto root = binding_json_parser(contents).parse();
            const auto& root_map = require_json_map(root, "$");
            const auto schema = optional_json_string(root_map, "schema", "$");
            if (schema != "canopy.rest.binding.v1")
                throw std::runtime_error("unsupported REST binding JSON schema: " + schema);

            const auto* interfaces_value = find_json_member(root_map, "interfaces");
            if (!interfaces_value)
                throw std::runtime_error("REST binding JSON is missing required field: $.interfaces");

            std::map<std::string, rest_interface> interfaces;
            const auto& interface_array = require_json_array(*interfaces_value, "$.interfaces");
            for (size_t interface_index = 0; interface_index < interface_array.size(); ++interface_index)
            {
                const auto interface_path = "$.interfaces[" + std::to_string(interface_index) + "]";
                const auto& interface_map = require_json_map(interface_array[interface_index], interface_path);
                rest_interface item;
                item.qualified_name = require_json_string(interface_map, "qualified_name", interface_path);
                item.host = optional_json_string(interface_map, "host", interface_path);
                item.base_path = optional_json_string(interface_map, "base_path", interface_path);

                const auto* methods_value = find_json_member(interface_map, "methods");
                if (!methods_value)
                    throw std::runtime_error("REST binding JSON is missing required field: " + interface_path + ".methods");
                const auto& method_array = require_json_array(*methods_value, interface_path + ".methods");
                for (size_t method_index = 0; method_index < method_array.size(); ++method_index)
                {
                    const auto method_path = interface_path + ".methods[" + std::to_string(method_index) + "]";
                    const auto& method_map = require_json_map(method_array[method_index], method_path);
                    rest_method method;
                    method.name = require_json_string(method_map, "name", method_path);
                    method.http_method = require_json_string(method_map, "http_method", method_path);
                    method.path = require_json_string(method_map, "path", method_path);

                    if (const auto* request_value = find_json_member(method_map, "request"))
                    {
                        const auto& request_map = require_json_map(*request_value, method_path + ".request");
                        method.body_param = optional_json_string(request_map, "body_param", method_path + ".request");
                        method.body_field_map
                            = optional_json_string(request_map, "body_field_map", method_path + ".request");
                        method.request_fields = read_json_response_fields(request_map, method_path + ".request");
                    }
                    if (const auto* response_value = find_json_member(method_map, "response"))
                    {
                        const auto& response_map = require_json_map(*response_value, method_path + ".response");
                        method.out_param = optional_json_string(response_map, "out_param", method_path + ".response");
                        method.response_field_map
                            = optional_json_string(response_map, "body_field_map", method_path + ".response");
                        method.response_content_type
                            = optional_json_string(response_map, "content_type", method_path + ".response");
                        method.response_status_code
                            = optional_json_int(response_map, "status_code", method_path + ".response");
                        if (method.response_status_code
                            && (method.response_status_code < 100 || method.response_status_code > 599))
                            throw std::runtime_error(
                                method_path + ".response.status_code must be a valid HTTP status code");
                        method.response_fields = read_json_response_fields(response_map, method_path + ".response");
                    }
                    method.parameters = read_json_parameters(method_map, method_path);
                    method.constants = read_json_constants(method_map, method_path);
                    item.methods[method.name] = std::move(method);
                }

                interfaces[item.qualified_name] = std::move(item);
            }

            return interfaces;
        }

        std::map<
            std::string,
            rest_interface>
        read_metadata(const std::filesystem::path& path)
        {
            if (path.extension() == ".json")
                return read_json_metadata(path);
            return read_tab_metadata(path);
        }

        std::string schema_header_filename(std::string header_filename)
        {
            const auto extension = header_filename.rfind(".h");
            if (extension == std::string::npos)
                return header_filename;
            header_filename.insert(extension, "_schema");
            return header_filename;
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

        const parameter_entity& find_parameter(
            const function_entity& function,
            const std::string& name)
        {
            for (const auto& parameter : function.get_parameters())
            {
                if (parameter.get_name() == name)
                    return parameter;
            }
            throw std::runtime_error("REST metadata references unknown parameter: " + name);
        }

        std::string rest_component_type(
            const class_entity& interface_entity,
            const parameter_entity& parameter)
        {
            auto type = render_cpp_type(interface_entity, parameter.get_type());
            if (type.rfind("const ", 0) == 0)
                type.erase(0, 6);
            while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back())))
                type.pop_back();
            while (!type.empty() && type.back() == '&')
                type.pop_back();
            while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back())))
                type.pop_back();
            return type;
        }

        std::string lower_ascii(std::string value)
        {
            for (auto& ch : value)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return value;
        }

        std::string media_type_without_parameters(std::string content_type)
        {
            const auto semicolon = content_type.find(';');
            if (semicolon != std::string::npos)
                content_type.erase(semicolon);
            while (!content_type.empty() && std::isspace(static_cast<unsigned char>(content_type.front())))
                content_type.erase(content_type.begin());
            while (!content_type.empty() && std::isspace(static_cast<unsigned char>(content_type.back())))
                content_type.pop_back();
            return lower_ascii(std::move(content_type));
        }

        bool is_json_media_type(const std::string& content_type)
        {
            if (content_type.empty())
                return true;
            const auto media_type = media_type_without_parameters(content_type);
            return media_type == "application/json" || media_type == "application/*+json" || media_type == "*/*"
                   || (media_type.size() > 5 && media_type.compare(media_type.size() - 5, 5, "+json") == 0);
        }

        std::string response_content_type(const rest_method& method)
        {
            return method.response_content_type.empty() ? "application/json" : method.response_content_type;
        }

        int response_status_code(const rest_method& method)
        {
            if (method.response_status_code)
                return method.response_status_code;
            if (method.out_param.empty() && method.response_fields.empty())
                return 204;
            return 200;
        }

        bool is_raw_string_response(
            const rest_method& method,
            std::string_view out_type)
        {
            return !is_json_media_type(method.response_content_type) && method.response_field_map.empty()
                   && out_type == "std::string";
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

        std::string render_rest_local_argument_list(const function_entity& function)
        {
            std::string output;
            bool has_parameter = false;
            for (const auto& parameter : function.get_parameters())
            {
                if (has_parameter)
                    output += ", ";
                has_parameter = true;
                output += "__rest_" + parameter.get_name();
            }
            return output;
        }

        bool method_is_const(const function_entity& function)
        {
            return function.has_value("const");
        }

        bool is_presence_constant(
            const rest_method& method,
            const rest_constant& constant)
        {
            if (!constant.value.empty())
                return false;

            return std::any_of(
                method.parameters.begin(),
                method.parameters.end(),
                [&constant](const auto& parameter)
                { return parameter.location == constant.location && parameter.wire_name == constant.wire_name; });
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

        std::vector<std::string> out_parameter_names(const function_entity& function)
        {
            std::vector<std::string> names;
            for (const auto& parameter : function.get_parameters())
            {
                if (is_out_param(parameter))
                    names.push_back(parameter.get_name());
            }
            return names;
        }

        std::vector<std::string> rest_output_names(const rest_method& method)
        {
            std::vector<std::string> names;
            if (!method.out_param.empty())
                names.push_back(method.out_param);
            for (const auto& field : method.response_fields)
                names.push_back(field.name);
            return names;
        }

        void validate_rest_outputs(
            const function_entity& function,
            const rest_method& method)
        {
            if (!method.out_param.empty() && !method.response_fields.empty())
                throw std::runtime_error(
                    "REST metadata mixes whole-response and field response bindings for method: " + method.name);

            const auto idl_outputs = out_parameter_names(function);
            const auto rest_outputs = rest_output_names(method);
            if (rest_outputs.empty())
            {
                if (!idl_outputs.empty())
                    throw std::runtime_error(
                        "REST metadata omits response parameters for method with IDL out parameters: " + method.name);
                return;
            }

            if (idl_outputs.empty())
                throw std::runtime_error(
                    "REST metadata describes response parameters for method with no IDL out parameters: " + method.name);

            for (const auto& output : rest_outputs)
            {
                if (!has_out_parameter(function, output))
                    throw std::runtime_error(
                        "REST metadata references unknown response parameter for method: " + method.name);
            }
            for (const auto& output : idl_outputs)
            {
                if (std::find(rest_outputs.begin(), rest_outputs.end(), output) == rest_outputs.end())
                    throw std::runtime_error("REST metadata omits IDL response parameter for method: " + method.name);
            }
        }

        std::string field_bindings_initializer(const std::vector<rest_response_field>& fields)
        {
            std::string output = "{";
            bool first = true;
            for (const auto& field : fields)
            {
                if (!first)
                    output += ", ";
                first = false;
                output += "{";
                output += cpp_string_literal(field.name);
                output += ", ";
                output += cpp_string_literal(field.wire_name);
                output += ", ";
                output += field.required ? "true" : "false";
                output += "}";
            }
            output += "}";
            return output;
        }

        std::string request_fields_initializer(const rest_method& method)
        {
            return field_bindings_initializer(method.request_fields);
        }

        std::string response_fields_initializer(const rest_method& method)
        {
            return field_bindings_initializer(method.response_fields);
        }

        void write_request_parameter(
            writer& output,
            const rest_parameter& parameter)
        {
            if (parameter.location == "body")
                return;

            const auto wire_name = cpp_string_literal(parameter.wire_name);
            output("const auto __rest_{0} = ::canopy::rest::to_json_value({0});", parameter.name);

            if (parameter.location == "path")
            {
                output(
                    "__request.target = ::canopy::rest::replace_path_parameter(__request.target, {0}, "
                    "::canopy::rest::component_string(__rest_{1}));",
                    wire_name,
                    parameter.name);
            }
            else if (parameter.location == "query")
            {
                if (parameter.required)
                {
                    output(
                        "canopy::rest::append_query_parameter(__request.target, {0}, "
                        "::canopy::rest::component_string(__rest_{1}));",
                        wire_name,
                        parameter.name);
                }
                else
                {
                    output("if (!::canopy::rest::is_null(__rest_{0}))", parameter.name);
                    output("{{");
                    output(
                        "canopy::rest::append_query_parameter(__request.target, {0}, "
                        "::canopy::rest::component_string(__rest_{1}));",
                        wire_name,
                        parameter.name);
                    output("}}");
                }
            }
            else if (parameter.location == "header")
            {
                if (parameter.required)
                {
                    output(
                        "__request.headers.push_back({{ {0}, ::canopy::rest::component_string(__rest_{1}) }});",
                        wire_name,
                        parameter.name);
                }
                else
                {
                    output("if (!::canopy::rest::is_null(__rest_{0}))", parameter.name);
                    output("{{");
                    output(
                        "__request.headers.push_back({{ {0}, ::canopy::rest::component_string(__rest_{1}) }});",
                        wire_name,
                        parameter.name);
                    output("}}");
                }
            }
        }

        void write_request_constant(
            writer& output,
            const rest_method& method,
            const rest_constant& constant)
        {
            if (is_presence_constant(method, constant))
                return;

            const auto wire_name = cpp_string_literal(constant.wire_name);
            const auto value = cpp_string_literal(constant.value);
            if (constant.location == "query")
            {
                output("canopy::rest::append_query_parameter(__request.target, {0}, {1});", wire_name, value);
            }
            else if (constant.location == "header")
            {
                output("__request.headers.push_back({{ {0}, {1} }});", wire_name, value);
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
            const auto const_suffix = method_is_const(function) ? " const" : "";
            validate_rest_outputs(function, method);

            output.print_tabs();
            output.raw(
                "CORO_TASK({}) {}::rest_caller::{}({}){}\n", return_type, interface_name, method.name, parameter_list, const_suffix);
            output("{{");
            output("try");
            output("{{");
            output("streaming::http_client::request __request;");
            output("__request.method = {};", cpp_string_literal(method.http_method));
            output(
                "__request.target = canopy::rest::join_target(settings_.endpoint.base_path, {});",
                cpp_string_literal(method.path));

            for (const auto& parameter : method.parameters)
                write_request_parameter(output, parameter);
            for (const auto& constant : method.constants)
                write_request_constant(output, method, constant);

            if (!method.body_param.empty())
            {
                if (!method.body_field_map.empty())
                {
                    output(
                        "__request.body = ::canopy::rest::body_from_value({0}, {1});",
                        method.body_param,
                        cpp_string_literal(method.body_field_map));
                }
                else if (method.request_fields.empty())
                {
                    output("__request.body = ::canopy::rest::body_from_value({});", method.body_param);
                }
                else
                {
                    output(
                        "__request.body = ::canopy::rest::body_from_value({0}, {1});",
                        method.body_param,
                        request_fields_initializer(method));
                }
                output("::canopy::rest::add_json_headers(__request.headers, true);");
            }
            else
            {
                output("::canopy::rest::add_json_headers(__request.headers, false);");
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
                if (method.response_fields.empty())
                {
                    output("CO_RETURN rpc::error::OK();");
                }
                else
                {
                    if (method.response_field_map.empty())
                    {
                        output(
                            "const auto __rest_response = ::canopy::rest::response_fields_from_body({}, "
                            "__http.value.body);",
                            response_fields_initializer(method));
                    }
                    else
                    {
                        output(
                            "const auto __rest_response = ::canopy::rest::response_fields_from_body({}, "
                            "__http.value.body, {});",
                            response_fields_initializer(method),
                            cpp_string_literal(method.response_field_map));
                    }
                    for (const auto& field : method.response_fields)
                    {
                        const auto type = rest_component_type(interface_entity, find_parameter(function, field.name));
                        output(
                            "::canopy::rest::assign_response_field<{1}>({0}, __rest_response, {2}, {3});",
                            field.name,
                            type,
                            cpp_string_literal(field.name),
                            field.required ? "true" : "false");
                    }
                    output("CO_RETURN rpc::error::OK();");
                }
            }
            else
            {
                const auto out_type = rest_component_type(interface_entity, find_parameter(function, method.out_param));
                if (is_raw_string_response(method, out_type))
                {
                    output("{0} = std::move(__http.value.body);", method.out_param);
                }
                else if (method.response_field_map.empty())
                {
                    output("{0} = ::canopy::rest::body_to_value<{1}>(__http.value.body);", method.out_param, out_type);
                }
                else
                {
                    output(
                        "{0} = ::canopy::rest::body_to_value<{1}>(__http.value.body, {2});",
                        method.out_param,
                        out_type,
                        cpp_string_literal(method.response_field_map));
                }
                output("CO_RETURN rpc::error::OK();");
            }
            output("}}");
            output("catch (const std::exception&)");
            output("{{");
            output("CO_RETURN rpc::error::INVALID_DATA();");
            output("}}");
            output("}}");
            output("");
        }

        void write_rest_handler_parameter(
            writer& output,
            const class_entity& interface_entity,
            const function_entity& function,
            const rest_parameter& parameter)
        {
            if (parameter.location == "body")
                return;

            const auto wire_name = cpp_string_literal(parameter.wire_name);
            const auto type = rest_component_type(interface_entity, find_parameter(function, parameter.name));

            if (parameter.location == "path")
            {
                output(
                    "auto __rest_{0} = ::canopy::rest::path_input_value<{1}>(__path, {2}, {3});",
                    parameter.name,
                    type,
                    wire_name,
                    parameter.required ? "true" : "false");
            }
            else if (parameter.location == "query")
            {
                output(
                    "auto __rest_{0} = ::canopy::rest::query_input_value<{1}>(request.target, {2}, {3});",
                    parameter.name,
                    type,
                    wire_name,
                    parameter.required ? "true" : "false");
            }
            else if (parameter.location == "header")
            {
                output(
                    "auto __rest_{0} = ::canopy::rest::header_input_value<{1}>(request.headers, {2}, {3});",
                    parameter.name,
                    type,
                    wire_name,
                    parameter.required ? "true" : "false");
            }
        }

        void write_rest_handler_constant_match(
            writer& output,
            const rest_method& method,
            const rest_constant& constant)
        {
            const auto wire_name = cpp_string_literal(constant.wire_name);
            const auto value = cpp_string_literal(constant.value);
            if (constant.location == "query")
            {
                output("{{");
                output(
                    "const auto __canopy_rest_value = ::canopy::rest::first_query_parameter(request.target, {});",
                    wire_name);
                if (is_presence_constant(method, constant))
                    output("if (!__canopy_rest_value)");
                else
                    output("if (!__canopy_rest_value || *__canopy_rest_value != {})", value);
                output("__canopy_rest_constants_matched = false;");
                output("}}");
            }
            else if (constant.location == "header")
            {
                output("{{");
                output("const auto __canopy_rest_value = ::canopy::rest::first_header_value(request.headers, {});", wire_name);
                if (is_presence_constant(method, constant))
                    output("if (!__canopy_rest_value)");
                else
                    output("if (!__canopy_rest_value || *__canopy_rest_value != {})", value);
                output("__canopy_rest_constants_matched = false;");
                output("}}");
            }
        }

        void write_rest_handler_method(
            writer& output,
            const class_entity& interface_entity,
            const rest_method& method)
        {
            const auto& function = find_function(interface_entity, method.name);
            const auto argument_list = render_rest_local_argument_list(function);
            validate_rest_outputs(function, method);

            output("{{");
            output(
                "const auto __path = "
                "::canopy::rest::match_path_template(::canopy::http_utils::request_path(request.target), "
                "::canopy::rest::join_target(base_path_, {}));",
                cpp_string_literal(method.path));
            output("if (__path.matched)");
            output("{{");
            output("if (::canopy::http_utils::ascii_iequals(request.method, {}))", cpp_string_literal(method.http_method));
            output("{{");
            output("bool __canopy_rest_constants_matched = true;");
            for (const auto& constant : method.constants)
                write_rest_handler_constant_match(output, method, constant);
            output("if (__canopy_rest_constants_matched)");
            output("{{");
            output("__canopy_rest_path_matched = true;");
            output("if (!object_)");
            output("{{");
            output("CO_RETURN ::canopy::rest::error_response(503, \"REST implementation is not configured\");");
            output("}}");
            for (const auto& parameter : method.parameters)
                write_rest_handler_parameter(output, interface_entity, function, parameter);
            if (!method.body_param.empty())
            {
                const auto body_type = rest_component_type(interface_entity, find_parameter(function, method.body_param));
                if (!method.body_field_map.empty())
                {
                    output(
                        "auto __rest_{0} = ::canopy::rest::body_to_value<{1}>(request.body, {2});",
                        method.body_param,
                        body_type,
                        cpp_string_literal(method.body_field_map));
                }
                else if (method.request_fields.empty())
                {
                    output("auto __rest_{0} = ::canopy::rest::body_to_value<{1}>(request.body);", method.body_param, body_type);
                }
                else
                {
                    output(
                        "auto __rest_{0} = ::canopy::rest::body_to_value<{1}>(request.body, {2});",
                        method.body_param,
                        body_type,
                        request_fields_initializer(method));
                }
            }
            for (const auto& parameter : function.get_parameters())
            {
                if (!is_out_param(parameter))
                    continue;
                const auto type = rest_component_type(interface_entity, parameter);
                output("{0} __rest_{1}{{}};", type, parameter.get_name());
            }
            output("auto __canopy_rest_call_result = CO_AWAIT object_->{0}({1});", method.name, argument_list);
            output("if (rpc::error::is_error(__canopy_rest_call_result))");
            output("{{");
            output("CO_RETURN ::canopy::rest::error_response(500, \"REST RPC call failed\");");
            output("}}");
            if (method.out_param.empty())
            {
                if (method.response_fields.empty())
                {
                    output(
                        "CO_RETURN ::canopy::rest::server_response{{{}, \"application/json\", {{}}, {{}}}};",
                        response_status_code(method));
                }
                else
                {
                    output("json::v1::map __rest_response_fields;");
                    for (const auto& field : method.response_fields)
                    {
                        output(
                            "::canopy::rest::add_response_field(__rest_response_fields, {0}, __rest_{1}, {2});",
                            cpp_string_literal(field.name),
                            field.name,
                            field.required ? "true" : "false");
                    }
                    if (method.response_field_map.empty())
                    {
                        output(
                            "CO_RETURN ::canopy::rest::server_response{{{0}, {1}, "
                            "::canopy::rest::response_body_from_fields(std::move(__rest_response_fields), {2}), "
                            "{{}}}};",
                            response_status_code(method),
                            cpp_string_literal(response_content_type(method)),
                            response_fields_initializer(method));
                    }
                    else
                    {
                        output(
                            "CO_RETURN ::canopy::rest::server_response{{{0}, {1}, "
                            "::canopy::rest::response_body_from_fields(std::move(__rest_response_fields), {2}, {3}), "
                            "{{}}}};",
                            response_status_code(method),
                            cpp_string_literal(response_content_type(method)),
                            response_fields_initializer(method),
                            cpp_string_literal(method.response_field_map));
                    }
                }
            }
            else
            {
                const auto out_type = rest_component_type(interface_entity, find_parameter(function, method.out_param));
                if (is_raw_string_response(method, out_type))
                {
                    output(
                        "CO_RETURN ::canopy::rest::server_response{{{0}, {1}, __rest_{2}, {{}}}};",
                        response_status_code(method),
                        cpp_string_literal(response_content_type(method)),
                        method.out_param);
                }
                else if (method.response_field_map.empty())
                {
                    output(
                        "CO_RETURN ::canopy::rest::server_response{{{0}, {1}, "
                        "::canopy::rest::body_from_value(__rest_{2}), {{}}}};",
                        response_status_code(method),
                        cpp_string_literal(response_content_type(method)),
                        method.out_param);
                }
                else
                {
                    output(
                        "CO_RETURN ::canopy::rest::server_response{{{0}, {1}, "
                        "::canopy::rest::body_from_value(__rest_{2}, {3}), {{}}}};",
                        response_status_code(method),
                        cpp_string_literal(response_content_type(method)),
                        method.out_param,
                        cpp_string_literal(method.response_field_map));
                }
            }
            output("}}");
            output("}}");
            output("else");
            output("{{");
            output("__canopy_rest_path_matched = true;");
            output(
                "::canopy::rest::append_allowed_method(__canopy_rest_allowed_methods, {});",
                cpp_string_literal(method.http_method));
            output("}}");
            output("}}");
            output("}}");
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
                "std::shared_ptr<rpc::service> service)",
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
            output("auto stream = CO_AWAIT canopy::rest::connect_stream(settings, std::move(service));");
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
            output("");

            output("{0}::rest_handler::rest_handler(rpc::shared_ptr<{0}> object, std::string base_path)", interface_name);
            output("    : object_(std::move(object))");
            output("    , base_path_(std::move(base_path))");
            output("{{");
            output("if (base_path_.empty())");
            output("{{");
            output("base_path_ = {};", cpp_string_literal(metadata.base_path));
            output("}}");
            output("}}");
            output("");

            output("std::string_view {0}::rest_handler_info::default_name() noexcept", interface_name);
            output("{{");
            output("return {};", cpp_string_literal(interface_name));
            output("}}");
            output("");

            output("std::string_view {0}::rest_handler_info::default_host() noexcept", interface_name);
            output("{{");
            output("return {};", cpp_string_literal(metadata.host));
            output("}}");
            output("");

            output("std::string_view {0}::rest_handler_info::default_base_path() noexcept", interface_name);
            output("{{");
            output("return {};", cpp_string_literal(metadata.base_path));
            output("}}");
            output("");

            output("std::string_view {0}::rest_handler::base_path() const noexcept", interface_name);
            output("{{");
            output("return base_path_;");
            output("}}");
            output("");

            output(
                "CORO_TASK(std::optional<::canopy::rest::server_response>) {0}::rest_handler::handle("
                "const ::canopy::rest::server_request& request) const",
                interface_name);
            output("{{");
            output("try");
            output("{{");
            output("bool __canopy_rest_path_matched = false;");
            output("std::string __canopy_rest_allowed_methods;");
            for (const auto* method : ordered_rest_handler_methods(metadata))
                write_rest_handler_method(output, interface_entity, *method);
            output("if (__canopy_rest_path_matched)");
            output("{{");
            output("CO_RETURN ::canopy::rest::method_not_allowed_response(std::move(__canopy_rest_allowed_methods));");
            output("}}");
            output("CO_RETURN std::nullopt;");
            output("}}");
            output("catch (const std::exception&)");
            output("{{");
            output("CO_RETURN ::canopy::rest::error_response(400, \"Invalid REST request\");");
            output("}}");
            output("}}");
            output("");
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
        const auto schema_header = schema_header_filename(header_filename);
        writer output(output_stream);

        output("#include <algorithm>");
        output("#include <chrono>");
        output("#include <cctype>");
        output("#include <memory>");
        output("#include <optional>");
        output("#include <stdexcept>");
        output("#include <string>");
        output("#include <string_view>");
        output("#include <utility>");
        output("#include <vector>");
        output("");
        output("#include <canopy/rest/helpers.h>");
        output("#include <canopy/rest/server.h>");
        output("#include <json/json_dom.h>");
        output("#include <rpc/rpc.h>");
        output("#include <streaming/http_client/client.h>");
        output("#include \"{}\"", header_filename);
        output("#include \"{}\"", schema_header);
        output("");
        output("// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)");

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
