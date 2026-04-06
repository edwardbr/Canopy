/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "proto_generator.h"

#include <algorithm>
#include <cctype>

#include "coreclasses.h"
#include "helpers.h"

namespace proto_generator
{
    std::string trim_copy(std::string value)
    {
        auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) { return !is_space(ch); }));
        value.erase(
            std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !is_space(ch); }).base(),
            value.end());
        return value;
    }

    std::string normalise_cpp_type(std::string type_name)
    {
        std::string reference_modifiers;
        strip_reference_modifiers(type_name, reference_modifiers);
        type_name = trim_copy(type_name);

        if (type_name.rfind("const ", 0) == 0)
            type_name = trim_copy(type_name.substr(6));

        return type_name;
    }

    size_t extract_template_content(
        const std::string& type,
        size_t start_pos,
        std::string& content)
    {
        if (start_pos >= type.length() || type[start_pos] != '<')
            return std::string::npos;

        int bracket_count = 1;
        size_t pos = start_pos + 1;
        while (pos < type.length() && bracket_count > 0)
        {
            if (type[pos] == '<')
                bracket_count++;
            else if (type[pos] == '>')
                bracket_count--;
            pos++;
        }

        if (bracket_count == 0)
        {
            content = type.substr(start_pos + 1, pos - start_pos - 2);
            return pos;
        }
        return std::string::npos;
    }

    bool split_template_args(
        const std::string& args,
        std::string& first,
        std::string& second)
    {
        int bracket_count = 0;
        for (size_t i = 0; i < args.length(); ++i)
        {
            const char c = args[i];
            if (c == '<')
                bracket_count++;
            else if (c == '>')
                bracket_count--;
            else if (c == ',' && bracket_count == 0)
            {
                first = trim_copy(args.substr(0, i));
                second = trim_copy(args.substr(i + 1));
                return true;
            }
        }
        return false;
    }

    bool is_map_type(
        const std::string& type,
        std::string& prefix)
    {
        if (type.find("std::map<") == 0)
        {
            prefix = "std::map<";
            return true;
        }
        if (type.find("std::unordered_map<") == 0)
        {
            prefix = "std::unordered_map<";
            return true;
        }
        if (type.find("std::flat_map<") == 0)
        {
            prefix = "std::flat_map<";
            return true;
        }
        return false;
    }

    bool is_sequence_type(
        const std::string& type,
        std::string& prefix)
    {
        if (type.find("std::vector<") == 0)
        {
            prefix = "std::vector<";
            return true;
        }
        if (type.find("std::array<") == 0)
        {
            prefix = "std::array<";
            return true;
        }
        return false;
    }

    std::string cpp_scalar_to_proto_type(const std::string& type)
    {
        if (type == "error_code" || type == "int8_t" || type == "signed char" || type == "int16_t"
            || type == "short" || type == "short int" || type == "signed short" || type == "signed short int"
            || type == "int32_t" || type == "int" || type == "signed int" || type == "char"
            || type == "wchar_t" || type == "char16_t" || type == "char32_t")
            return "int32";

        if (type == "int64_t" || type == "long" || type == "long int" || type == "signed long"
            || type == "signed long int" || type == "long long" || type == "signed long long"
            || type == "long long int" || type == "signed long long int" || type == "ptrdiff_t" || type == "ssize_t"
            || type == "intptr_t")
            return "int64";

        if (type == "uint8_t" || type == "unsigned char" || type == "uint16_t" || type == "unsigned short"
            || type == "unsigned short int" || type == "uint32_t" || type == "unsigned int")
            return "uint32";

        if (type == "uint64_t" || type == "unsigned long" || type == "unsigned long int"
            || type == "unsigned long long" || type == "unsigned long long int" || type == "size_t"
            || type == "uintptr_t")
            return "uint64";

        if (type == "float")
            return "float";
        if (type == "double" || type == "long double")
            return "double";
        if (type == "bool")
            return "bool";
        if (type == "std::string" || type == "string" || type == "char*" || type == "const char*"
            || type == "char *" || type == "const char *")
            return "string";

        return "";
    }

    std::string sanitize_type_name(const std::string& type_name)
    {
        std::string result = type_name;

        size_t pos = 0;
        while ((pos = result.find("::", pos)) != std::string::npos)
        {
            result.replace(pos, 2, ".");
            pos += 1;
        }

        if (!result.empty() && !std::isalpha(static_cast<unsigned char>(result[0])) && result[0] != '_')
            result = "_" + result;

        for (auto& c : result)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.')
                c = '_';
        }

        return result;
    }

    std::string sanitize_field_name(const std::string& field_name)
    {
        std::string result = field_name;

        for (auto& c : result)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                c = '_';
        }

        if (!result.empty() && !std::isalpha(static_cast<unsigned char>(result[0])) && result[0] != '_')
            result = "_" + result;

        return result;
    }

    bool is_enum_type(
        const class_entity& lib,
        const std::string& type_name)
    {
        const auto normalized = normalise_cpp_type(type_name);

        std::function<bool(const class_entity&)> search_for_enum = [&](const class_entity& entity) -> bool
        {
            for (auto& elem : entity.get_elements(entity_type::ENUM))
            {
                if (elem && elem->get_name() == normalized)
                    return true;
            }

            for (auto& ns_entity : entity.get_elements(entity_type::NAMESPACE))
            {
                if (ns_entity && search_for_enum(static_cast<const class_entity&>(*ns_entity)))
                    return true;
            }

            return false;
        };

        return search_for_enum(lib);
    }

    std::string cpp_type_to_proto_type(const std::string& cpp_type)
    {
        return cpp_type_to_proto_type(
            cpp_type,
            [](const std::string&) { return false; },
            [](const std::string&) { return false; },
            [](const std::string& type_name) { return sanitize_type_name(type_name); });
    }

    std::string cpp_type_to_proto_type(
        const std::string& cpp_type,
        const std::function<bool(const std::string&)>& is_interface_type,
        const std::function<bool(const std::string&)>& is_enum_type_fn,
        const std::function<std::string(const std::string&)>& sanitize_custom_type)
    {
        std::string type = cpp_type;
        if (type.find("const ") == 0)
            type = type.substr(6);

        const bool is_pointer = type.find('*') != std::string::npos;

        size_t pos = type.find('&');
        if (pos != std::string::npos)
            type = type.substr(0, pos);

        pos = type.find('*');
        if (pos != std::string::npos)
            type = type.substr(0, pos);

        type = trim_copy(type);

        if (is_pointer)
            return "uint64";

        if (type == "std::vector<uint8_t>" || type == "std::vector<unsigned char>" || type == "std::vector<char>"
            || type == "std::vector<signed char>")
            return "bytes";

        std::string container_prefix;
        if (is_map_type(type, container_prefix))
        {
            const size_t template_start = type.find('<');
            std::string inner_content;
            if (extract_template_content(type, template_start, inner_content) != std::string::npos)
            {
                std::string key_type;
                std::string value_type;
                if (split_template_args(inner_content, key_type, value_type))
                {
                    return "map<" + cpp_type_to_proto_type(key_type, is_interface_type, is_enum_type_fn, sanitize_custom_type)
                         + ", " + cpp_type_to_proto_type(value_type, is_interface_type, is_enum_type_fn, sanitize_custom_type) + ">";
                }
            }
            return "map<string, string>";
        }

        if (is_sequence_type(type, container_prefix))
        {
            const size_t template_start = type.find('<');
            std::string inner_content;
            if (extract_template_content(type, template_start, inner_content) != std::string::npos)
            {
                if (container_prefix == "std::array<")
                {
                    std::string element_type;
                    std::string size_param;
                    if (split_template_args(inner_content, element_type, size_param))
                    {
                        return "repeated "
                             + cpp_type_to_proto_type(element_type, is_interface_type, is_enum_type_fn, sanitize_custom_type);
                    }
                }
                return "repeated " + cpp_type_to_proto_type(inner_content, is_interface_type, is_enum_type_fn, sanitize_custom_type);
            }
            return "repeated string";
        }

        if (is_interface_type(type))
            return "rpc.remote_object";

        if (type == "unsigned __int128" || type == "__int128" || type == "uint128_t" || type == "int128_t")
            return "uint128";

        const auto scalar_type = cpp_scalar_to_proto_type(type);
        if (!scalar_type.empty())
            return scalar_type;

        if (is_enum_type_fn(type))
            return sanitize_custom_type(type);

        const size_t template_start = type.find('<');
        if (template_start != std::string::npos && type.find('>') != std::string::npos)
        {
            std::string template_name = type.substr(0, template_start);
            std::string inner_content;

            if (extract_template_content(type, template_start, inner_content) != std::string::npos)
            {
                std::string sanitized_suffix = inner_content;

                if (inner_content == "int" || inner_content == "int32_t")
                    sanitized_suffix = "int";
                else if (inner_content == "uint32_t" || inner_content == "unsigned int")
                    sanitized_suffix = "uint";
                else if (inner_content == "int64_t" || inner_content == "long" || inner_content == "long long")
                    sanitized_suffix = "int64";
                else if (inner_content == "uint64_t" || inner_content == "unsigned long"
                         || inner_content == "unsigned long long")
                    sanitized_suffix = "uint64";
                else if (inner_content == "int16_t" || inner_content == "short")
                    sanitized_suffix = "int16";
                else if (inner_content == "uint16_t" || inner_content == "unsigned short")
                    sanitized_suffix = "uint16";
                else if (inner_content == "int8_t" || inner_content == "signed char")
                    sanitized_suffix = "int8";
                else if (inner_content == "uint8_t" || inner_content == "unsigned char")
                    sanitized_suffix = "uint8";
                else if (inner_content == "std::string" || inner_content == "string")
                    sanitized_suffix = "string";
                else if (inner_content == "float")
                    sanitized_suffix = "float";
                else if (inner_content == "double")
                    sanitized_suffix = "double";
                else if (inner_content == "bool")
                    sanitized_suffix = "bool";
                else
                    sanitized_suffix = sanitize_custom_type(inner_content);

                return template_name + "_" + sanitized_suffix;
            }
        }

        return sanitize_custom_type(type);
    }
}
