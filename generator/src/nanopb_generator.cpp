/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "nanopb_generator.h"

#include "coreclasses.h"
#include "cpp_parser.h"
#include "helpers.h"
#include "proto_generator.h"
#include "type_utils.h"
#include "writer.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace nanopb_generator
{
    struct TemplateInstantiation
    {
        std::string template_name;
        std::string template_param;
        std::string concrete_name;

        bool operator<(const TemplateInstantiation& other) const
        {
            if (template_name != other.template_name)
                return template_name < other.template_name;
            if (template_param != other.template_param)
                return template_param < other.template_param;
            return concrete_name < other.concrete_name;
        }
    };

    bool is_interface_type(const std::string& type);
    bool is_scalar_type(const std::string& type);

    std::string normalize_type(const std::string& type_str)
    {
        std::string cleaned_type = type_str;
        size_t const_pos = cleaned_type.find("const ");
        while (const_pos != std::string::npos)
        {
            cleaned_type.erase(const_pos, 6);
            const_pos = cleaned_type.find("const ");
        }

        while (!cleaned_type.empty()
               && (cleaned_type.back() == '&' || cleaned_type.back() == '*' || cleaned_type.back() == ' '))
        {
            cleaned_type.pop_back();
        }

        while (!cleaned_type.empty() && cleaned_type.front() == ' ')
            cleaned_type.erase(0, 1);

        return cleaned_type;
    }

    bool is_string_type(const std::string& type)
    {
        return normalize_type(type) == "std::string";
    }

    bool is_byte_vector_type(const std::string& type)
    {
        const auto normalized = normalize_type(type);
        return normalized == "std::vector<uint8_t>" || normalized == "std::vector<unsigned char>"
               || normalized == "std::vector<char>" || normalized == "std::vector<signed char>";
    }

    bool is_unsigned_byte_vector_type(const std::string& type)
    {
        const auto normalized = normalize_type(type);
        return normalized == "std::vector<uint8_t>" || normalized == "std::vector<unsigned char>";
    }

    bool is_json_dom_type(const std::string& type)
    {
        return proto_generator::is_json_dom_type(normalize_type(type));
    }

    bool is_pointer_type(const std::string& type)
    {
        return type.find('*') != std::string::npos;
    }

    std::string pointer_cast_type(const std::string& type)
    {
        std::string cleaned = type;
        while (!cleaned.empty() && cleaned.front() == ' ')
            cleaned.erase(0, 1);
        while (!cleaned.empty() && cleaned.back() == ' ')
            cleaned.pop_back();

        while (!cleaned.empty() && cleaned.back() == '&')
        {
            cleaned.pop_back();
            while (!cleaned.empty() && cleaned.back() == ' ')
                cleaned.pop_back();
        }

        return cleaned;
    }

    bool is_sequence_type(const std::string& type)
    {
        std::string prefix;
        return proto_generator::is_sequence_type(normalize_type(type), prefix);
    }

    bool is_vector_type(const std::string& type)
    {
        std::string prefix;
        return proto_generator::is_sequence_type(normalize_type(type), prefix) && prefix == "std::vector<";
    }

    bool is_map_type(const std::string& type)
    {
        std::string prefix;
        return proto_generator::is_map_type(normalize_type(type), prefix);
    }

    bool is_int128_type(const std::string& type)
    {
        const auto normalized = normalize_type(type);
        return normalized == "__int128" || normalized == "unsigned __int128" || normalized == "int128_t"
               || normalized == "uint128_t";
    }

    bool is_template_type(const std::string& type)
    {
        const auto normalized = normalize_type(type);
        return normalized.find('<') != std::string::npos && normalized.find('>') != std::string::npos;
    }

    bool is_optional_type(const std::string& type)
    {
        std::string inner_type;
        return proto_generator::is_optional_type(normalize_type(type), inner_type);
    }

    std::string optional_inner_type(const std::string& type)
    {
        return proto_generator::optional_inner_type(normalize_type(type));
    }

    bool is_variant_type(
        const std::string& type,
        std::vector<std::string>& alternative_types)
    {
        return proto_generator::is_variant_type(normalize_type(type), alternative_types);
    }

    std::string variant_wrapper_message_name(
        const std::string& owning_message_name,
        const std::string& field_name)
    {
        return proto_generator::sanitize_type_name(owning_message_name) + "_" + field_name + "_variant";
    }

    std::string variant_value_field_name(size_t alternative_index)
    {
        return "value_" + std::to_string(alternative_index);
    }

    bool is_user_template_instantiation(const std::string& type)
    {
        const auto normalized = normalize_type(type);
        const auto template_start = normalized.find('<');
        if (template_start == std::string::npos || normalized.find('>') == std::string::npos)
            return false;

        const auto template_name = normalized.substr(0, template_start);
        return template_name.rfind("std::", 0) != 0 && template_name.rfind("rpc::", 0) != 0;
    }

    std::string vector_inner_type(const std::string& type)
    {
        const auto normalized = normalize_type(type);
        const auto start = normalized.find('<');
        const auto end = normalized.rfind('>');
        if (start == std::string::npos || end == std::string::npos || end <= start)
            return {};
        auto inner = normalized.substr(start + 1, end - start - 1);
        while (!inner.empty() && inner.front() == ' ')
            inner.erase(0, 1);
        while (!inner.empty() && inner.back() == ' ')
            inner.pop_back();
        return inner;
    }

    bool map_key_value_types(
        const std::string& type,
        std::string& key_type,
        std::string& value_type)
    {
        const auto normalized = normalize_type(type);
        const auto start = normalized.find('<');
        const auto end = normalized.rfind('>');
        if (start == std::string::npos || end == std::string::npos || end <= start)
            return false;

        return proto_generator::split_template_args(normalized.substr(start + 1, end - start - 1), key_type, value_type);
    }

    bool split_user_template_instantiation(
        const std::string& type,
        std::string& template_name,
        std::string& template_param,
        std::string& concrete_name)
    {
        const auto normalized = normalize_type(type);
        const auto template_start = normalized.find('<');
        if (template_start == std::string::npos || normalized.find('>') == std::string::npos)
            return false;

        template_name = normalized.substr(0, template_start);
        if (template_name.rfind("std::", 0) == 0 || template_name.rfind("rpc::", 0) == 0)
            return false;

        int bracket_count = 1;
        size_t pos = template_start + 1;
        while (pos < normalized.length() && bracket_count > 0)
        {
            if (normalized[pos] == '<')
                ++bracket_count;
            else if (normalized[pos] == '>')
                --bracket_count;
            ++pos;
        }

        if (bracket_count != 0)
            return false;

        template_param = normalized.substr(template_start + 1, pos - template_start - 2);
        concrete_name = proto_generator::cpp_type_to_proto_type(normalized);
        return true;
    }

    bool is_repeated_varint_type(const std::string& type)
    {
        if (!is_vector_type(type) || is_byte_vector_type(type))
            return false;
        const auto inner = vector_inner_type(type);
        return inner == "int" || inner == "int8_t" || inner == "int16_t" || inner == "int32_t" || inner == "int64_t"
               || inner == "uint8_t" || inner == "uint16_t" || inner == "uint32_t" || inner == "uint64_t"
               || inner == "unsigned int" || inner == "bool" || inner == "size_t";
    }

    bool is_sequence_structure_type(const std::string& type)
    {
        if (!is_vector_type(type) || is_byte_vector_type(type))
            return false;
        const auto inner = vector_inner_type(type);
        return !inner.empty() && !is_scalar_type(inner) && !is_string_type(inner) && !is_int128_type(inner)
               && !is_template_type(inner) && !is_interface_type(inner);
    }

    bool is_dictionary_structure_type(const std::string& type)
    {
        if (!is_map_type(type))
            return false;

        std::string key_type;
        std::string value_type;
        if (!map_key_value_types(type, key_type, value_type))
            return false;

        key_type = normalize_type(key_type);
        value_type = normalize_type(value_type);
        return key_type == "std::string" && !value_type.empty() && !is_scalar_type(value_type)
               && !is_string_type(value_type) && !is_int128_type(value_type) && !is_template_type(value_type)
               && !is_interface_type(value_type);
    }

    bool is_unsupported_nanopb_field_type(const std::string& type)
    {
        if (is_optional_type(type))
            return is_unsupported_nanopb_field_type(optional_inner_type(type));
        std::vector<std::string> variant_alternatives;
        if (is_variant_type(type, variant_alternatives))
        {
            return std::any_of(
                variant_alternatives.begin(),
                variant_alternatives.end(),
                [](const auto& alternative_type) { return is_unsupported_nanopb_field_type(alternative_type); });
        }
        if (is_interface_type(type) || is_byte_vector_type(type) || is_json_dom_type(type)
            || is_repeated_varint_type(type) || is_sequence_structure_type(type) || is_dictionary_structure_type(type))
            return false;
        return is_sequence_type(type) || is_map_type(type) || is_int128_type(type)
               || (is_template_type(type) && !is_user_template_instantiation(type));
    }

    bool is_scalar_type(const std::string& type)
    {
        static const std::vector<std::string> scalars = {"int",
            "int8_t",
            "int16_t",
            "int32_t",
            "int64_t",
            "uint8_t",
            "uint16_t",
            "uint32_t",
            "uint64_t",
            "unsigned int",
            "signed int",
            "short",
            "unsigned short",
            "signed short",
            "long",
            "unsigned long",
            "signed long",
            "long long",
            "unsigned long long",
            "signed long long",
            "char",
            "unsigned char",
            "signed char",
            "bool",
            "float",
            "double",
            "size_t",
            "ptrdiff_t",
            "error_code"};
        const auto normalized = normalize_type(type);
        return std::find(scalars.begin(), scalars.end(), normalized) != scalars.end();
    }

    const class_entity& root_entity(const class_entity& entity)
    {
        auto* current = &entity;
        while (current->get_owner())
            current = current->get_owner();
        return *current;
    }

    std::string qualified_entity_name(const class_entity& entity)
    {
        std::vector<std::string> names;
        for (auto* current = &entity; current; current = current->get_owner())
        {
            if (!current->get_name().empty())
                names.push_back(current->get_name());
        }

        std::string result;
        for (auto it = names.rbegin(); it != names.rend(); ++it)
        {
            if (!result.empty())
                result += "::";
            result += *it;
        }
        return result;
    }

    bool is_interface_type(const std::string& type)
    {
        return type.find("rpc::shared_ptr") != std::string::npos || type.find("rpc::optimistic_ptr") != std::string::npos
               || type.find("rpc::remote_object") != std::string::npos;
    }

    std::string proxy_input_type(const std::string& param_type)
    {
        if (is_interface_type(param_type))
            return "const rpc::remote_object&";
        if (param_type.find('*') != std::string::npos)
            return "uint64_t";
        if (param_type.find("&&") != std::string::npos)
        {
            std::string base_type = param_type;
            std::string reference_modifiers;
            generator::strip_reference_modifiers(base_type, reference_modifiers);
            return "const " + base_type + "&";
        }
        if (param_type.find('&') != std::string::npos)
        {
            if (param_type.find("const") == std::string::npos)
                return "const " + param_type;
            return param_type;
        }
        return "const " + param_type + "&";
    }

    std::string proxy_output_type(const std::string& param_type)
    {
        if (is_interface_type(param_type))
            return "rpc::remote_object&";
        if (param_type.find('*') != std::string::npos)
            return "uint64_t&";
        if (param_type.find("&&") != std::string::npos)
            return param_type;
        if (param_type.find('&') != std::string::npos)
            return param_type;
        return param_type + "&";
    }

    std::string stub_input_type(const std::string& param_type)
    {
        if (is_interface_type(param_type))
            return "rpc::remote_object&";
        if (param_type.find('*') != std::string::npos)
            return "uint64_t&";
        if (param_type.find("&&") != std::string::npos)
        {
            std::string base_type = param_type.substr(0, param_type.find("&&"));
            while (!base_type.empty() && base_type.back() == ' ')
                base_type.pop_back();
            return base_type + "&";
        }
        if (param_type.find('&') != std::string::npos)
        {
            std::string clean_type = param_type;
            size_t const_pos = clean_type.find("const ");
            if (const_pos != std::string::npos)
                clean_type.erase(const_pos, 6);
            return clean_type;
        }
        return param_type + "&";
    }

    std::string stub_output_type(const std::string& param_type)
    {
        if (is_interface_type(param_type))
            return "rpc::remote_object&";
        if (param_type.find('*') != std::string::npos)
            return "uint64_t";
        if (param_type.find("&&") != std::string::npos)
        {
            std::string base_type = param_type.substr(0, param_type.find("&&"));
            while (!base_type.empty() && base_type.back() == ' ')
                base_type.pop_back();
            return "const " + base_type + "&";
        }
        if (param_type.find('&') != std::string::npos)
        {
            if (param_type.find("const ") == std::string::npos)
                return "const " + param_type;
            return param_type;
        }
        return "const " + param_type + "&";
    }

    std::string namespace_name(const class_entity& current_lib)
    {
        std::string prefix;
        if (current_lib.get_owner())
            prefix = namespace_name(*current_lib.get_owner());

        if (!prefix.empty())
            prefix += "_";
        prefix += current_lib.get_name();
        return prefix;
    }

    std::string nanopb_c_prefix(const std::string& package_name)
    {
        if (package_name.empty())
            return "protobuf";
        return "protobuf_" + package_name;
    }

    std::string concrete_message_name(const class_entity& struct_entity)
    {
        return proto_generator::sanitize_type_name(struct_entity.get_name());
    }

    std::string unqualified_type_name(const std::string& type)
    {
        auto normalized = normalize_type(type);
        const auto namespace_pos = normalized.rfind("::");
        if (namespace_pos != std::string::npos)
            normalized = normalized.substr(namespace_pos + 2);
        return proto_generator::sanitize_type_name(normalized);
    }

    std::string nanopb_c_type_for_cpp_type(
        const std::string& package_name,
        const std::string& type)
    {
        if (is_interface_type(type))
            return "protobuf_rpc_remote_object";

        const auto normalized = normalize_type(type);
        if (normalized.rfind("rpc::", 0) == 0)
            return "protobuf_rpc_" + unqualified_type_name(normalized);
        if (is_user_template_instantiation(normalized))
            return nanopb_c_prefix(package_name) + "_" + proto_generator::cpp_type_to_proto_type(normalized);

        return nanopb_c_prefix(package_name) + "_" + unqualified_type_name(normalized);
    }

    const class_entity* find_enum_entity(
        const class_entity& lib,
        const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);
        if (norm_type.rfind("::", 0) == 0)
            norm_type.erase(0, 2);

        std::string unqualified = norm_type;
        size_t ns_pos = norm_type.rfind("::");
        if (ns_pos != std::string::npos)
            unqualified = norm_type.substr(ns_pos + 2);

        std::function<const class_entity*(const class_entity&)> search_for_enum
            = [&](const class_entity& entity) -> const class_entity*
        {
            for (auto& elem : entity.get_elements(entity_type::ENUM))
            {
                if (!elem)
                    continue;

                auto* enum_entity = dynamic_cast<const class_entity*>(elem.get());
                const auto qualified_name = enum_entity ? qualified_entity_name(*enum_entity) : elem->get_name();
                if (qualified_name == norm_type || elem->get_name() == unqualified)
                    return enum_entity;
            }

            for (auto& elem : entity.get_elements(entity_type::NAMESPACE))
            {
                auto& ns_entity = static_cast<const class_entity&>(*elem);
                if (const auto* found = search_for_enum(ns_entity))
                    return found;
            }

            return nullptr;
        };

        return search_for_enum(root_entity(lib));
    }

    const class_entity* find_error_entity(
        const class_entity& lib,
        const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);
        if (norm_type.rfind("::", 0) == 0)
            norm_type.erase(0, 2);

        std::string unqualified = norm_type;
        size_t ns_pos = norm_type.rfind("::");
        if (ns_pos != std::string::npos)
            unqualified = norm_type.substr(ns_pos + 2);

        std::function<const class_entity*(const class_entity&)> search_for_error
            = [&](const class_entity& entity) -> const class_entity*
        {
            for (auto& elem : entity.get_elements(entity_type::ERROR))
            {
                if (!elem)
                    continue;

                auto* error_entity = dynamic_cast<const class_entity*>(elem.get());
                const auto qualified_name = error_entity ? qualified_entity_name(*error_entity) : elem->get_name();
                if (qualified_name == norm_type || elem->get_name() == unqualified)
                    return error_entity;
            }

            for (auto& elem : entity.get_elements(entity_type::NAMESPACE))
            {
                auto& ns_entity = static_cast<const class_entity&>(*elem);
                if (const auto* found = search_for_error(ns_entity))
                    return found;
            }

            return nullptr;
        };

        return search_for_error(root_entity(lib));
    }

    bool is_enum_type(
        const class_entity& lib,
        const std::string& type_str)
    {
        return find_enum_entity(lib, type_str) != nullptr;
    }

    bool is_error_type(
        const class_entity& lib,
        const std::string& type_str)
    {
        return find_error_entity(lib, type_str) != nullptr;
    }

    std::string enum_type_ref(
        const class_entity& lib,
        const std::string& type)
    {
        if (const auto* enum_entity = find_enum_entity(lib, type))
            return qualified_entity_name(*enum_entity);
        return normalize_type(type);
    }

    bool has_explicit_access_markers(const class_entity& entity)
    {
        for (const auto& member : entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_PRIVATE
                || member->get_entity_type() == entity_type::FUNCTION_PUBLIC)
                return true;
        }
        return false;
    }

    const class_entity* find_struct_entity(
        const class_entity& lib,
        const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);
        if (norm_type.rfind("::", 0) == 0)
            norm_type.erase(0, 2);

        std::string unqualified = norm_type;
        size_t ns_pos = norm_type.rfind("::");
        if (ns_pos != std::string::npos)
            unqualified = norm_type.substr(ns_pos + 2);

        std::function<const class_entity*(const class_entity&)> search_for_struct
            = [&](const class_entity& entity) -> const class_entity*
        {
            for (auto& elem : entity.get_elements(entity_type::STRUCT))
            {
                if (!elem)
                    continue;

                auto* struct_entity = dynamic_cast<const class_entity*>(elem.get());
                if (!struct_entity)
                    continue;

                const auto qualified_name = qualified_entity_name(*struct_entity);
                if (qualified_name == norm_type || elem->get_name() == unqualified)
                    return struct_entity;
            }

            for (auto& elem : entity.get_elements(entity_type::NAMESPACE))
            {
                auto& ns_entity = static_cast<const class_entity&>(*elem);
                if (const auto* found = search_for_struct(ns_entity))
                    return found;
            }

            return nullptr;
        };

        return search_for_struct(root_entity(lib));
    }

    const class_entity* find_struct_or_template_entity(
        const class_entity& lib,
        const std::string& type_str,
        std::string* concrete_template_param = nullptr)
    {
        if (concrete_template_param)
            concrete_template_param->clear();

        std::string template_name;
        std::string template_param;
        std::string concrete_name;
        if (split_user_template_instantiation(type_str, template_name, template_param, concrete_name))
        {
            if (concrete_template_param)
                *concrete_template_param = template_param;
            return find_struct_entity(lib, template_name);
        }

        return find_struct_entity(lib, type_str);
    }

    bool optional_wrapper_value_has_nanopb_presence(
        const class_entity& lib,
        const std::string& inner_type)
    {
        const auto normalized = normalize_type(inner_type);

        if (is_scalar_type(normalized) || is_string_type(normalized) || is_byte_vector_type(normalized)
            || is_json_dom_type(normalized) || is_pointer_type(normalized) || is_interface_type(normalized)
            || is_enum_type(lib, normalized) || is_error_type(lib, normalized))
            return false;

        if (is_repeated_varint_type(normalized) || is_sequence_structure_type(normalized)
            || is_dictionary_structure_type(normalized) || is_sequence_type(normalized) || is_map_type(normalized)
            || is_int128_type(normalized))
            return false;

        // Nanopb emits a has_value member for message fields. That lets the
        // generated Canopy layer reject a present optional wrapper that omitted
        // the wrapped message entirely.
        return true;
    }

    std::string substitute_single_template_parameter(
        const class_entity& template_entity,
        const std::string& concrete_param,
        const std::string& type);

    void write_cpp_to_nanopb_message(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        const std::string& source_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp,
        const std::string& concrete_template_param = {});

    void write_unsupported_member(writer& cpp);

    void write_prepare_nanopb_message_decode(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp,
        const std::string& concrete_template_param = {});

    void write_nanopb_message_to_cpp(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        const std::string& source_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp,
        const std::string& concrete_template_param = {});

    void write_cpp_to_nanopb_field(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& member_name,
        const std::string& member_type,
        const std::string& source_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp);

    void write_cpp_value_to_nanopb_field(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& field_name,
        const std::string& field_type,
        const std::string& source_value_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        if (is_pointer_type(field_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>(reinterpret_cast<std::uintptr_t>({}));",
                target_expr,
                field_name,
                target_expr,
                field_name,
                source_value_expr);
        }
        else if (is_enum_type(lib, field_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>(static_cast<std::underlying_type_t<{}>>({}));",
                target_expr,
                field_name,
                target_expr,
                field_name,
                enum_type_ref(lib, field_type),
                source_value_expr);
        }
        else if (is_error_type(lib, field_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>(static_cast<int>({}));",
                target_expr,
                field_name,
                target_expr,
                field_name,
                source_value_expr);
        }
        else if (is_scalar_type(field_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>({});", target_expr, field_name, target_expr, field_name, source_value_expr);
        }
        else if (is_string_type(field_type))
        {
            cpp("rpc::serialization::nanopb::string_encode_state {}_state {{ &{} }};", state_prefix, source_value_expr);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_string;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_byte_vector_type(field_type))
        {
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state {{ {}.data(), {}.size() }};",
                state_prefix,
                source_value_expr,
                source_value_expr);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_json_dom_type(field_type))
        {
            cpp("std::vector<char> {}_buffer;", state_prefix);
            cpp("{}.nanopb_serialise({}_buffer);", source_value_expr, state_prefix);
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state {{ {}_buffer.data(), {}_buffer.size() }};",
                state_prefix,
                state_prefix,
                state_prefix);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_repeated_varint_type(field_type))
        {
            const auto inner_type = vector_inner_type(field_type);
            cpp("rpc::serialization::nanopb::repeated_varint_encode_state<{}> {}_state {{ &{} }};",
                inner_type,
                state_prefix,
                source_value_expr);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_repeated_varint<{}>;",
                target_expr,
                field_name,
                inner_type);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_sequence_structure_type(field_type))
        {
            const auto inner_type = vector_inner_type(field_type);
            if (const auto* nested_struct = find_struct_entity(lib, inner_type);
                nested_struct && !has_explicit_access_markers(*nested_struct))
            {
                const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, inner_type);
                cpp("rpc::serialization::nanopb::sequence_submessage_encode_state<{}> {}_state {{ &{}, {}_fields,",
                    inner_type,
                    state_prefix,
                    source_value_expr,
                    nested_c_type);
                cpp("    +[](pb_ostream_t* __stream, const pb_msgdesc_t* __fields, const {}& __value) -> bool", inner_type);
                cpp("    {{");
                cpp("        {} __message = {}_init_zero;", nested_c_type, nested_c_type);
                write_cpp_to_nanopb_message(
                    lib, *nested_struct, package_name, "__value", "__message", state_prefix + "_item", cpp);
                cpp("        return pb_encode_submessage(__stream, __fields, &__message);");
                cpp("    }} }};");
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_sequence_submessage<{}>;",
                    target_expr,
                    field_name,
                    inner_type);
            }
            else
            {
                cpp("rpc::serialization::nanopb::sequence_structure_encode_state<{}> {}_state {{ &{} }};",
                    inner_type,
                    state_prefix,
                    source_value_expr);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_sequence_structure<{}>;",
                    target_expr,
                    field_name,
                    inner_type);
            }
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_dictionary_structure_type(field_type))
        {
            std::string key_type;
            std::string value_type;
            const bool has_map_types = map_key_value_types(field_type, key_type, value_type);
            const auto normalized_field_type = normalize_type(field_type);
            const auto* nested_struct = has_map_types ? find_struct_entity(lib, value_type) : nullptr;
            if (nested_struct && !has_explicit_access_markers(*nested_struct))
            {
                const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, value_type);
                cpp("rpc::serialization::nanopb::dictionary_submessage_encode_state<{}> {}_state {{ &{}, {}_fields,",
                    normalized_field_type,
                    state_prefix,
                    source_value_expr,
                    nested_c_type);
                cpp("    +[](pb_ostream_t* __stream, const pb_msgdesc_t* __fields, const {}& __value) -> bool",
                    normalize_type(value_type));
                cpp("    {{");
                cpp("        {} __message = {}_init_zero;", nested_c_type, nested_c_type);
                write_cpp_to_nanopb_message(
                    lib, *nested_struct, package_name, "__value", "__message", state_prefix + "_value", cpp);
                cpp("        return pb_encode_submessage(__stream, __fields, &__message);");
                cpp("    }} }};");
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_dictionary_submessage<{}>;",
                    target_expr,
                    field_name,
                    normalized_field_type);
            }
            else
            {
                cpp("rpc::serialization::nanopb::dictionary_structure_encode_state<{}> {}_state {{ &{} }};",
                    normalized_field_type,
                    state_prefix,
                    source_value_expr);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_dictionary_structure<{}>;",
                    target_expr,
                    field_name,
                    normalized_field_type);
            }
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else
        {
            std::string nested_concrete_template_param;
            if (const auto* nested_struct
                = find_struct_or_template_entity(lib, field_type, &nested_concrete_template_param))
            {
                if (!has_explicit_access_markers(*nested_struct))
                {
                    write_cpp_to_nanopb_message(
                        lib,
                        *nested_struct,
                        package_name,
                        source_value_expr,
                        target_expr + "." + field_name,
                        state_prefix + "_" + field_name,
                        cpp,
                        nested_concrete_template_param);
                }
                else
                {
                    const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, field_type);
                    cpp("{{");
                    cpp("std::vector<char> {}_buffer;", state_prefix);
                    cpp("{}.nanopb_serialise({}_buffer);", source_value_expr, state_prefix);
                    cpp("rpc::serialization::nanopb::decode_message({}_buffer, {}_fields, &{}.{});",
                        state_prefix,
                        nested_c_type,
                        target_expr,
                        field_name);
                    cpp("}}");
                }
                cpp("{}.has_{} = true;", target_expr, field_name);
            }
            else
            {
                write_unsupported_member(cpp);
            }
        }
    }

    void write_optional_nanopb_encode_state_storage(
        const std::string& field_type,
        const std::string& state_prefix,
        writer& cpp)
    {
        if (is_string_type(field_type))
        {
            cpp("rpc::serialization::nanopb::string_encode_state {}_state;", state_prefix);
        }
        else if (is_byte_vector_type(field_type))
        {
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state;", state_prefix);
        }
        else if (is_json_dom_type(field_type))
        {
            cpp("std::vector<char> {}_buffer;", state_prefix);
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state;", state_prefix);
        }
    }

    void write_optional_cpp_value_to_nanopb_field(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& field_name,
        const std::string& field_type,
        const std::string& source_value_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        if (is_string_type(field_type))
        {
            cpp("{}_state = rpc::serialization::nanopb::string_encode_state {{ &{} }};", state_prefix, source_value_expr);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_string;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_byte_vector_type(field_type))
        {
            cpp("{}_state = rpc::serialization::nanopb::bytes_encode_state {{ {}.data(), {}.size() }};",
                state_prefix,
                source_value_expr,
                source_value_expr);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_json_dom_type(field_type))
        {
            cpp("{}.nanopb_serialise({}_buffer);", source_value_expr, state_prefix);
            cpp("{}_state = rpc::serialization::nanopb::bytes_encode_state {{ {}_buffer.data(), {}_buffer.size() }};",
                state_prefix,
                state_prefix,
                state_prefix);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else
        {
            write_cpp_value_to_nanopb_field(
                lib, package_name, field_name, field_type, source_value_expr, target_expr, state_prefix, cpp);
        }
    }

    void write_prepare_nanopb_field_decode(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& field_name,
        const std::string& field_type,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        if (is_string_type(field_type))
        {
            cpp("std::string {}_decoded;", state_prefix);
            cpp("rpc::serialization::nanopb::string_decode_state {}_state {{ &{}_decoded }};", state_prefix, state_prefix);
            cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_string;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_byte_vector_type(field_type))
        {
            cpp("std::vector<uint8_t> {}_decoded;", state_prefix);
            cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};", state_prefix, state_prefix);
            cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_json_dom_type(field_type))
        {
            cpp("std::vector<uint8_t> {}_decoded;", state_prefix);
            cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};", state_prefix, state_prefix);
            cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", target_expr, field_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_repeated_varint_type(field_type))
        {
            const auto inner_type = vector_inner_type(field_type);
            cpp("{} {}_decoded;", normalize_type(field_type), state_prefix);
            cpp("rpc::serialization::nanopb::repeated_varint_decode_state<{}> {}_state {{ &{}_decoded }};",
                inner_type,
                state_prefix,
                state_prefix);
            cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_repeated_varint<{}>;",
                target_expr,
                field_name,
                inner_type);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_sequence_structure_type(field_type))
        {
            const auto inner_type = vector_inner_type(field_type);
            cpp("{} {}_decoded;", normalize_type(field_type), state_prefix);
            cpp("rpc::serialization::nanopb::sequence_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                inner_type,
                state_prefix,
                state_prefix);
            cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_sequence_structure<{}>;",
                target_expr,
                field_name,
                inner_type);
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else if (is_dictionary_structure_type(field_type))
        {
            cpp("{} {}_decoded;", normalize_type(field_type), state_prefix);
            cpp("rpc::serialization::nanopb::dictionary_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                normalize_type(field_type),
                state_prefix,
                state_prefix);
            cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_dictionary_structure<{}>;",
                target_expr,
                field_name,
                normalize_type(field_type));
            cpp("{}.{}.arg = &{}_state;", target_expr, field_name, state_prefix);
        }
        else
        {
            std::string nested_concrete_template_param;
            if (const auto* nested_struct
                = find_struct_or_template_entity(lib, field_type, &nested_concrete_template_param))
            {
                write_prepare_nanopb_message_decode(
                    lib, *nested_struct, package_name, target_expr + "." + field_name, state_prefix, cpp, nested_concrete_template_param);
            }
        }
    }

    void write_nanopb_field_to_cpp_value(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& field_name,
        const std::string& field_type,
        const std::string& source_expr,
        const std::string& dest_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        if (is_pointer_type(field_type))
        {
            cpp("{} = reinterpret_cast<{}>(static_cast<std::uintptr_t>({}.{}));",
                dest_expr,
                pointer_cast_type(field_type),
                source_expr,
                field_name);
        }
        else if (is_enum_type(lib, field_type))
        {
            cpp("{} = static_cast<{}>(static_cast<std::underlying_type_t<{}>>({}.{}));",
                dest_expr,
                enum_type_ref(lib, field_type),
                enum_type_ref(lib, field_type),
                source_expr,
                field_name);
        }
        else if (is_error_type(lib, field_type))
        {
            cpp("{} = static_cast<int>({}.{});", dest_expr, source_expr, field_name);
        }
        else if (is_scalar_type(field_type))
        {
            cpp("{} = static_cast<{}>({}.{});", dest_expr, normalize_type(field_type), source_expr, field_name);
        }
        else if (is_string_type(field_type))
        {
            cpp("{} = std::move({}_decoded);", dest_expr, state_prefix);
        }
        else if (is_byte_vector_type(field_type))
        {
            if (is_unsigned_byte_vector_type(field_type))
                cpp("{} = std::move({}_decoded);", dest_expr, state_prefix);
            else
                cpp("{}.assign({}_decoded.begin(), {}_decoded.end());", dest_expr, state_prefix, state_prefix);
        }
        else if (is_json_dom_type(field_type))
        {
            cpp("std::vector<char> {}_buffer({}_decoded.begin(), {}_decoded.end());", state_prefix, state_prefix, state_prefix);
            cpp("{}.nanopb_deserialise({}_buffer);", dest_expr, state_prefix);
        }
        else if (is_repeated_varint_type(field_type) || is_sequence_structure_type(field_type)
                 || is_dictionary_structure_type(field_type))
        {
            cpp("{} = std::move({}_decoded);", dest_expr, state_prefix);
        }
        else
        {
            std::string nested_concrete_template_param;
            if (const auto* nested_struct
                = find_struct_or_template_entity(lib, field_type, &nested_concrete_template_param))
            {
                write_nanopb_message_to_cpp(
                    lib,
                    *nested_struct,
                    package_name,
                    source_expr + "." + field_name,
                    dest_expr,
                    state_prefix,
                    cpp,
                    nested_concrete_template_param);
            }
            else
            {
                write_unsupported_member(cpp);
            }
        }
    }

    void write_prepare_nanopb_variant_decode(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& variant_type,
        const std::string& wrapper_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        std::vector<std::string> alternative_types;
        if (!is_variant_type(variant_type, alternative_types))
            return;

        for (size_t index = 0; index < alternative_types.size(); ++index)
        {
            write_prepare_nanopb_field_decode(
                lib,
                package_name,
                variant_value_field_name(index),
                alternative_types[index],
                wrapper_expr,
                state_prefix + "_" + variant_value_field_name(index),
                cpp);
        }
    }

    void write_cpp_variant_to_nanopb_field(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& field_name,
        const std::string& variant_type,
        const std::string& source_value_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        std::vector<std::string> alternative_types;
        if (!is_variant_type(variant_type, alternative_types))
            return;

        cpp("if ({}.valueless_by_exception())", source_value_expr);
        cpp("{{");
        cpp("throw std::runtime_error(\"Cannot serialize valueless variant\");");
        cpp("}}");

        for (size_t index = 0; index < alternative_types.size(); ++index)
        {
            const auto current_prefix = state_prefix + "_" + variant_value_field_name(index);
            if (is_string_type(alternative_types[index]))
            {
                cpp("rpc::serialization::nanopb::string_encode_state {}_state;", current_prefix);
            }
            else if (is_byte_vector_type(alternative_types[index]))
            {
                cpp("rpc::serialization::nanopb::bytes_encode_state {}_state;", current_prefix);
            }
            else if (is_json_dom_type(alternative_types[index]))
            {
                cpp("std::vector<char> {}_buffer;", current_prefix);
                cpp("rpc::serialization::nanopb::bytes_encode_state {}_state;", current_prefix);
            }
        }

        cpp("{}.{}.index = static_cast<uint32_t>({}.index());", target_expr, field_name, source_value_expr);
        cpp("switch ({}.index())", source_value_expr);
        cpp("{{");
        for (size_t index = 0; index < alternative_types.size(); ++index)
        {
            const auto value_field_name = variant_value_field_name(index);
            const auto current_prefix = state_prefix + "_" + value_field_name;
            const auto alternative_expr = "rpc::get<" + std::to_string(index) + ">(" + source_value_expr + ")";
            const auto wrapper_expr = target_expr + "." + field_name;

            cpp("case {}:", index);
            cpp("{{");
            if (is_string_type(alternative_types[index]))
            {
                cpp("{}_state = rpc::serialization::nanopb::string_encode_state {{ &{} }};", current_prefix, alternative_expr);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_string;", wrapper_expr, value_field_name);
                cpp("{}.{}.arg = &{}_state;", wrapper_expr, value_field_name, current_prefix);
            }
            else if (is_byte_vector_type(alternative_types[index]))
            {
                cpp("{}_state = rpc::serialization::nanopb::bytes_encode_state {{ {}.data(), {}.size() }};",
                    current_prefix,
                    alternative_expr,
                    alternative_expr);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", wrapper_expr, value_field_name);
                cpp("{}.{}.arg = &{}_state;", wrapper_expr, value_field_name, current_prefix);
            }
            else if (is_json_dom_type(alternative_types[index]))
            {
                cpp("{}.nanopb_serialise({}_buffer);", alternative_expr, current_prefix);
                cpp("{}_state = rpc::serialization::nanopb::bytes_encode_state {{ {}_buffer.data(), "
                    "{}_buffer.size() }};",
                    current_prefix,
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", wrapper_expr, value_field_name);
                cpp("{}.{}.arg = &{}_state;", wrapper_expr, value_field_name, current_prefix);
            }
            else
            {
                write_cpp_value_to_nanopb_field(
                    lib,
                    package_name,
                    value_field_name,
                    alternative_types[index],
                    alternative_expr,
                    wrapper_expr,
                    current_prefix,
                    cpp);
            }
            cpp("break;");
            cpp("}}");
        }
        cpp("default:");
        cpp("throw std::runtime_error(\"Unknown variant alternative\");");
        cpp("}}");
        cpp("{}.has_{} = true;", target_expr, field_name);
    }

    void write_nanopb_variant_to_cpp_value(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& variant_type,
        const std::string& source_wrapper_expr,
        const std::string& dest_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        std::vector<std::string> alternative_types;
        if (!is_variant_type(variant_type, alternative_types))
            return;

        cpp("switch ({}.index)", source_wrapper_expr);
        cpp("{{");
        for (size_t index = 0; index < alternative_types.size(); ++index)
        {
            cpp("case {}:", index);
            cpp("{{");
            cpp("{} __variant_value {{}};", normalize_type(alternative_types[index]));
            write_nanopb_field_to_cpp_value(
                lib,
                package_name,
                variant_value_field_name(index),
                alternative_types[index],
                source_wrapper_expr,
                "__variant_value",
                state_prefix + "_" + variant_value_field_name(index),
                cpp);
            cpp("{} = std::move(__variant_value);", dest_expr);
            cpp("break;");
            cpp("}}");
        }
        cpp("default:");
        cpp("{} = {}{{}};", dest_expr, normalize_type(variant_type));
        cpp("break;");
        cpp("}}");
    }

    void write_cpp_to_nanopb_field(
        const class_entity& lib,
        const std::string& package_name,
        const std::string& member_name,
        const std::string& member_type,
        const std::string& source_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp)
    {
        if (is_optional_type(member_type))
        {
            const auto inner_type = optional_inner_type(member_type);
            write_optional_nanopb_encode_state_storage(inner_type, state_prefix + "_value", cpp);
            cpp("if ({}.{}.has_value())", source_expr, member_name);
            cpp("{{");
            write_optional_cpp_value_to_nanopb_field(
                lib,
                package_name,
                "value",
                inner_type,
                source_expr + "." + member_name + ".value()",
                target_expr + "." + member_name,
                state_prefix + "_value",
                cpp);
            cpp("{}.has_{} = true;", target_expr, member_name);
            cpp("}}");
            cpp("else");
            cpp("{{");
            cpp("{}.has_{} = false;", target_expr, member_name);
            cpp("}}");
        }
        else if (std::vector<std::string> variant_alternatives; is_variant_type(member_type, variant_alternatives))
        {
            write_cpp_variant_to_nanopb_field(
                lib, package_name, member_name, member_type, source_expr + "." + member_name, target_expr, state_prefix, cpp);
        }
        else if (is_pointer_type(member_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>(reinterpret_cast<std::uintptr_t>({}.{}));",
                target_expr,
                member_name,
                target_expr,
                member_name,
                source_expr,
                member_name);
        }
        else if (is_enum_type(lib, member_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>(static_cast<std::underlying_type_t<{}>>({}.{}));",
                target_expr,
                member_name,
                target_expr,
                member_name,
                enum_type_ref(lib, member_type),
                source_expr,
                member_name);
        }
        else if (is_error_type(lib, member_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>(static_cast<int>({}.{}));",
                target_expr,
                member_name,
                target_expr,
                member_name,
                source_expr,
                member_name);
        }
        else if (is_scalar_type(member_type))
        {
            cpp("{}.{} = static_cast<decltype({}.{})>({}.{});",
                target_expr,
                member_name,
                target_expr,
                member_name,
                source_expr,
                member_name);
        }
        else if (is_string_type(member_type))
        {
            cpp("rpc::serialization::nanopb::string_encode_state {}_state {{ &{}.{} }};", state_prefix, source_expr, member_name);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_string;", target_expr, member_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, member_name, state_prefix);
        }
        else if (is_byte_vector_type(member_type))
        {
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state {{ {}.{}.data(), {}.{}.size() }};",
                state_prefix,
                source_expr,
                member_name,
                source_expr,
                member_name);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", target_expr, member_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, member_name, state_prefix);
        }
        else if (is_json_dom_type(member_type))
        {
            cpp("std::vector<char> {}_buffer;", state_prefix);
            cpp("{}.{}.nanopb_serialise({}_buffer);", source_expr, member_name, state_prefix);
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state {{ {}_buffer.data(), {}_buffer.size() }};",
                state_prefix,
                state_prefix,
                state_prefix);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", target_expr, member_name);
            cpp("{}.{}.arg = &{}_state;", target_expr, member_name, state_prefix);
        }
        else if (is_repeated_varint_type(member_type))
        {
            const auto inner_type = vector_inner_type(member_type);
            cpp("rpc::serialization::nanopb::repeated_varint_encode_state<{}> {}_state {{ &{}.{} }};",
                inner_type,
                state_prefix,
                source_expr,
                member_name);
            cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_repeated_varint<{}>;",
                target_expr,
                member_name,
                inner_type);
            cpp("{}.{}.arg = &{}_state;", target_expr, member_name, state_prefix);
        }
        else if (is_sequence_structure_type(member_type))
        {
            const auto inner_type = vector_inner_type(member_type);
            if (const auto* nested_struct = find_struct_entity(lib, inner_type);
                nested_struct && !has_explicit_access_markers(*nested_struct))
            {
                const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, inner_type);
                cpp("rpc::serialization::nanopb::sequence_submessage_encode_state<{}> {}_state {{ &{}.{}, {}_fields,",
                    inner_type,
                    state_prefix,
                    source_expr,
                    member_name,
                    nested_c_type);
                cpp("    +[](pb_ostream_t* __stream, const pb_msgdesc_t* __fields, const {}& __value) -> bool", inner_type);
                cpp("    {{");
                cpp("        {} __message = {}_init_zero;", nested_c_type, nested_c_type);
                write_cpp_to_nanopb_message(
                    lib, *nested_struct, package_name, "__value", "__message", state_prefix + "_item", cpp);
                cpp("        return pb_encode_submessage(__stream, __fields, &__message);");
                cpp("    }} }};");
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_sequence_submessage<{}>;",
                    target_expr,
                    member_name,
                    inner_type);
            }
            else
            {
                cpp("rpc::serialization::nanopb::sequence_structure_encode_state<{}> {}_state {{ &{}.{} }};",
                    inner_type,
                    state_prefix,
                    source_expr,
                    member_name);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_sequence_structure<{}>;",
                    target_expr,
                    member_name,
                    inner_type);
            }
            cpp("{}.{}.arg = &{}_state;", target_expr, member_name, state_prefix);
        }
        else if (is_dictionary_structure_type(member_type))
        {
            std::string key_type;
            std::string value_type;
            const bool has_map_types = map_key_value_types(member_type, key_type, value_type);
            const auto normalized_member_type = normalize_type(member_type);
            const auto* nested_struct = has_map_types ? find_struct_entity(lib, value_type) : nullptr;
            if (nested_struct && !has_explicit_access_markers(*nested_struct))
            {
                const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, value_type);
                cpp("rpc::serialization::nanopb::dictionary_submessage_encode_state<{}> {}_state {{ &{}.{}, {}_fields,",
                    normalized_member_type,
                    state_prefix,
                    source_expr,
                    member_name,
                    nested_c_type);
                cpp("    +[](pb_ostream_t* __stream, const pb_msgdesc_t* __fields, const {}& __value) -> bool",
                    normalize_type(value_type));
                cpp("    {{");
                cpp("        {} __message = {}_init_zero;", nested_c_type, nested_c_type);
                write_cpp_to_nanopb_message(
                    lib, *nested_struct, package_name, "__value", "__message", state_prefix + "_value", cpp);
                cpp("        return pb_encode_submessage(__stream, __fields, &__message);");
                cpp("    }} }};");
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_dictionary_submessage<{}>;",
                    target_expr,
                    member_name,
                    normalized_member_type);
            }
            else
            {
                cpp("rpc::serialization::nanopb::dictionary_structure_encode_state<{}> {}_state {{ &{}.{} }};",
                    normalized_member_type,
                    state_prefix,
                    source_expr,
                    member_name);
                cpp("{}.{}.funcs.encode = rpc::serialization::nanopb::encode_dictionary_structure<{}>;",
                    target_expr,
                    member_name,
                    normalized_member_type);
            }
            cpp("{}.{}.arg = &{}_state;", target_expr, member_name, state_prefix);
        }
        else
        {
            std::string nested_concrete_template_param;
            if (const auto* nested_struct
                = find_struct_or_template_entity(lib, member_type, &nested_concrete_template_param))
            {
                if (!has_explicit_access_markers(*nested_struct))
                {
                    write_cpp_to_nanopb_message(
                        lib,
                        *nested_struct,
                        package_name,
                        source_expr + "." + member_name,
                        target_expr + "." + member_name,
                        state_prefix + "_" + member_name,
                        cpp,
                        nested_concrete_template_param);
                }
                else
                {
                    const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, member_type);
                    cpp("{{");
                    cpp("std::vector<char> {}_buffer;", state_prefix);
                    cpp("{}.{}.nanopb_serialise({}_buffer);", source_expr, member_name, state_prefix);
                    cpp("rpc::serialization::nanopb::decode_message({}_buffer, {}_fields, &{}.{});",
                        state_prefix,
                        nested_c_type,
                        target_expr,
                        member_name);
                    cpp("}}");
                }
                cpp("{}.has_{} = true;", target_expr, member_name);
            }
            else
            {
                write_unsupported_member(cpp);
            }
        }
    }

    void write_cpp_to_nanopb_message(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        const std::string& source_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp,
        const std::string& concrete_template_param)
    {
        for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            auto member_type = field->get_return_type();
            if (!concrete_template_param.empty())
                member_type = substitute_single_template_parameter(struct_entity, concrete_template_param, member_type);

            write_cpp_to_nanopb_field(
                lib,
                package_name,
                proto_generator::sanitize_field_name(field->get_name()),
                member_type,
                source_expr,
                target_expr,
                state_prefix + "_" + proto_generator::sanitize_field_name(field->get_name()),
                cpp);
        }
    }

    void write_prepare_nanopb_message_decode(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp,
        const std::string& concrete_template_param)
    {
        for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = proto_generator::sanitize_field_name(field->get_name());
            auto member_type = field->get_return_type();
            if (!concrete_template_param.empty())
                member_type = substitute_single_template_parameter(struct_entity, concrete_template_param, member_type);
            const auto current_prefix = state_prefix + "_" + member_name;

            if (is_optional_type(member_type))
            {
                write_prepare_nanopb_field_decode(
                    lib,
                    package_name,
                    "value",
                    optional_inner_type(member_type),
                    target_expr + "." + member_name,
                    current_prefix + "_value",
                    cpp);
            }
            else if (std::vector<std::string> variant_alternatives; is_variant_type(member_type, variant_alternatives))
            {
                write_prepare_nanopb_variant_decode(
                    lib, package_name, member_type, target_expr + "." + member_name, current_prefix, cpp);
            }
            else if (is_string_type(member_type))
            {
                cpp("std::string {}_decoded;", current_prefix);
                cpp("rpc::serialization::nanopb::string_decode_state {}_state {{ &{}_decoded }};",
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_string;", target_expr, member_name);
                cpp("{}.{}.arg = &{}_state;", target_expr, member_name, current_prefix);
            }
            else if (is_byte_vector_type(member_type))
            {
                cpp("std::vector<uint8_t> {}_decoded;", current_prefix);
                cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};",
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", target_expr, member_name);
                cpp("{}.{}.arg = &{}_state;", target_expr, member_name, current_prefix);
            }
            else if (is_json_dom_type(member_type))
            {
                cpp("std::vector<uint8_t> {}_decoded;", current_prefix);
                cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};",
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", target_expr, member_name);
                cpp("{}.{}.arg = &{}_state;", target_expr, member_name, current_prefix);
            }
            else if (is_repeated_varint_type(member_type))
            {
                const auto inner_type = vector_inner_type(member_type);
                cpp("{} {}_decoded;", normalize_type(member_type), current_prefix);
                cpp("rpc::serialization::nanopb::repeated_varint_decode_state<{}> {}_state {{ &{}_decoded }};",
                    inner_type,
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_repeated_varint<{}>;",
                    target_expr,
                    member_name,
                    inner_type);
                cpp("{}.{}.arg = &{}_state;", target_expr, member_name, current_prefix);
            }
            else if (is_sequence_structure_type(member_type))
            {
                const auto inner_type = vector_inner_type(member_type);
                cpp("{} {}_decoded;", normalize_type(member_type), current_prefix);
                cpp("rpc::serialization::nanopb::sequence_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                    inner_type,
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_sequence_structure<{}>;",
                    target_expr,
                    member_name,
                    inner_type);
                cpp("{}.{}.arg = &{}_state;", target_expr, member_name, current_prefix);
            }
            else if (is_dictionary_structure_type(member_type))
            {
                cpp("{} {}_decoded;", normalize_type(member_type), current_prefix);
                cpp("rpc::serialization::nanopb::dictionary_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                    normalize_type(member_type),
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.funcs.decode = rpc::serialization::nanopb::decode_dictionary_structure<{}>;",
                    target_expr,
                    member_name,
                    normalize_type(member_type));
                cpp("{}.{}.arg = &{}_state;", target_expr, member_name, current_prefix);
            }
            else
            {
                std::string nested_concrete_template_param;
                if (const auto* nested_struct
                    = find_struct_or_template_entity(lib, member_type, &nested_concrete_template_param))
                {
                    write_prepare_nanopb_message_decode(
                        lib,
                        *nested_struct,
                        package_name,
                        target_expr + "." + member_name,
                        current_prefix,
                        cpp,
                        nested_concrete_template_param);
                }
            }
        }
    }

    void write_nanopb_message_to_cpp(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        const std::string& source_expr,
        const std::string& target_expr,
        const std::string& state_prefix,
        writer& cpp,
        const std::string& concrete_template_param)
    {
        for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = proto_generator::sanitize_field_name(field->get_name());
            auto member_type = field->get_return_type();
            if (!concrete_template_param.empty())
                member_type = substitute_single_template_parameter(struct_entity, concrete_template_param, member_type);
            const auto current_prefix = state_prefix + "_" + member_name;

            if (is_optional_type(member_type))
            {
                const auto inner_type = optional_inner_type(member_type);
                cpp("if ({}.has_{})", source_expr, member_name);
                cpp("{{");
                if (optional_wrapper_value_has_nanopb_presence(lib, inner_type))
                {
                    cpp("if (!{}.{}.has_value)", source_expr, member_name);
                    cpp("{{");
                    cpp("throw std::runtime_error(\"Malformed nanopb optional field {}: wrapper is present without "
                        "value\");",
                        member_name);
                    cpp("}}");
                }
                cpp("{} {}_value {{}};", normalize_type(inner_type), current_prefix);
                write_nanopb_field_to_cpp_value(
                    lib,
                    package_name,
                    "value",
                    inner_type,
                    source_expr + "." + member_name,
                    current_prefix + "_value",
                    current_prefix + "_value",
                    cpp);
                cpp("{}.{} = {}_value;", target_expr, member_name, current_prefix);
                cpp("}}");
                cpp("else");
                cpp("{{");
                cpp("{}.{}.reset();", target_expr, member_name);
                cpp("}}");
            }
            else if (std::vector<std::string> variant_alternatives; is_variant_type(member_type, variant_alternatives))
            {
                if (optional_wrapper_value_has_nanopb_presence(lib, member_type))
                {
                    cpp("if (!{}.has_{})", source_expr, member_name);
                    cpp("{{");
                    cpp("{} = {}{{}};", target_expr + "." + member_name, normalize_type(member_type));
                    cpp("}}");
                    cpp("else");
                    cpp("{{");
                    write_nanopb_variant_to_cpp_value(
                        lib,
                        package_name,
                        member_type,
                        source_expr + "." + member_name,
                        target_expr + "." + member_name,
                        current_prefix,
                        cpp);
                    cpp("}}");
                }
                else
                {
                    write_nanopb_variant_to_cpp_value(
                        lib,
                        package_name,
                        member_type,
                        source_expr + "." + member_name,
                        target_expr + "." + member_name,
                        current_prefix,
                        cpp);
                }
            }
            else if (is_pointer_type(member_type))
            {
                cpp("{}.{} = reinterpret_cast<{}>(static_cast<std::uintptr_t>({}.{}));",
                    target_expr,
                    member_name,
                    pointer_cast_type(member_type),
                    source_expr,
                    member_name);
            }
            else if (is_enum_type(lib, member_type))
            {
                cpp("{}.{} = static_cast<{}>(static_cast<std::underlying_type_t<{}>>({}.{}));",
                    target_expr,
                    member_name,
                    enum_type_ref(lib, member_type),
                    enum_type_ref(lib, member_type),
                    source_expr,
                    member_name);
            }
            else if (is_error_type(lib, member_type))
            {
                cpp("{}.{} = static_cast<int>({}.{});", target_expr, member_name, source_expr, member_name);
            }
            else if (is_scalar_type(member_type))
            {
                cpp("{}.{} = static_cast<{}>({}.{});",
                    target_expr,
                    member_name,
                    normalize_type(member_type),
                    source_expr,
                    member_name);
            }
            else if (is_string_type(member_type))
            {
                cpp("{}.{} = std::move({}_decoded);", target_expr, member_name, current_prefix);
            }
            else if (is_byte_vector_type(member_type))
            {
                if (is_unsigned_byte_vector_type(member_type))
                    cpp("{}.{} = std::move({}_decoded);", target_expr, member_name, current_prefix);
                else
                    cpp("{}.{}.assign({}_decoded.begin(), {}_decoded.end());",
                        target_expr,
                        member_name,
                        current_prefix,
                        current_prefix);
            }
            else if (is_json_dom_type(member_type))
            {
                cpp("std::vector<char> {}_buffer({}_decoded.begin(), {}_decoded.end());",
                    current_prefix,
                    current_prefix,
                    current_prefix);
                cpp("{}.{}.nanopb_deserialise({}_buffer);", target_expr, member_name, current_prefix);
            }
            else if (is_repeated_varint_type(member_type) || is_sequence_structure_type(member_type)
                     || is_dictionary_structure_type(member_type))
            {
                cpp("{}.{} = std::move({}_decoded);", target_expr, member_name, current_prefix);
            }
            else
            {
                std::string nested_concrete_template_param;
                if (const auto* nested_struct
                    = find_struct_or_template_entity(lib, member_type, &nested_concrete_template_param))
                {
                    write_nanopb_message_to_cpp(
                        lib,
                        *nested_struct,
                        package_name,
                        source_expr + "." + member_name,
                        target_expr + "." + member_name,
                        current_prefix,
                        cpp,
                        nested_concrete_template_param);
                }
                else
                {
                    write_unsupported_member(cpp);
                }
            }
        }
    }

    void write_unsupported_member(writer& cpp)
    {
        cpp("throw std::runtime_error(\"Nanopb serialization is not implemented for this IDL field category\");");
    }

    std::string substitute_single_template_parameter(
        const class_entity& template_entity,
        const std::string& concrete_param,
        const std::string& type)
    {
        const auto& template_params = template_entity.get_template_params();
        if (template_params.empty())
            return type;

        const auto template_param_name = template_params.front().get_name();
        return normalize_type(type) == template_param_name ? concrete_param : type;
    }

    void collect_template_instantiations(
        const class_entity& lib,
        std::set<TemplateInstantiation>& instantiations)
    {
        for (const auto& interface_elem : lib.get_elements(entity_type::INTERFACE))
        {
            auto& interface_entity = static_cast<const class_entity&>(*interface_elem);

            for (const auto& function : interface_entity.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                for (const auto& parameter : function->get_parameters())
                {
                    if (!is_in_param(parameter) && is_out_param(parameter))
                        continue;

                    TemplateInstantiation inst;
                    if (split_user_template_instantiation(
                            parameter.get_type(), inst.template_name, inst.template_param, inst.concrete_name))
                        instantiations.insert(inst);
                }
            }
        }

        for (const auto& struct_elem : lib.get_elements(entity_type::STRUCT))
        {
            auto& struct_entity = static_cast<const class_entity&>(*struct_elem);
            if (struct_entity.get_is_template())
                continue;

            for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
            {
                if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;

                auto field = std::static_pointer_cast<function_entity>(member);
                if (field->is_static())
                    continue;

                TemplateInstantiation inst;
                if (split_user_template_instantiation(
                        field->get_return_type(), inst.template_name, inst.template_param, inst.concrete_name))
                    instantiations.insert(inst);
            }
        }
    }

    void write_template_instantiation_methods(
        const class_entity& lib,
        const class_entity& template_entity,
        const TemplateInstantiation& inst,
        const std::string& package_name,
        writer& cpp)
    {
        const auto struct_name = template_entity.get_name();
        const auto c_type = nanopb_c_prefix(package_name) + "_" + inst.concrete_name;

        cpp("template<>");
        cpp("void {}<{}>::nanopb_serialise(std::vector<char>& buffer) const", struct_name, inst.template_param);
        cpp("{{");
        cpp("{} msg = {}_init_zero;", c_type, c_type);
        for (const auto& member : template_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = field->get_name();
            const auto member_type
                = substitute_single_template_parameter(template_entity, inst.template_param, field->get_return_type());
            write_cpp_to_nanopb_field(
                lib, package_name, proto_generator::sanitize_field_name(member_name), member_type, "(*this)", "msg", member_name, cpp);
        }
        cpp("rpc::serialization::nanopb::encode_message(buffer, {}_fields, &msg);", c_type);
        cpp("}}");
        cpp("");

        cpp("template<>");
        cpp("void {}<{}>::nanopb_deserialise(const std::vector<char>& buffer)", struct_name, inst.template_param);
        cpp("{{");
        cpp("{} msg = {}_init_zero;", c_type, c_type);
        cpp("rpc::serialization::nanopb::decode_message(buffer, {}_fields, &msg);", c_type);
        for (const auto& member : template_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = field->get_name();
            const auto field_name = proto_generator::sanitize_field_name(member_name);
            const auto member_type
                = substitute_single_template_parameter(template_entity, inst.template_param, field->get_return_type());

            if (is_optional_type(member_type))
            {
                const auto inner_type = optional_inner_type(member_type);
                cpp("if (msg.has_{})", field_name);
                cpp("{{");
                if (optional_wrapper_value_has_nanopb_presence(lib, inner_type))
                {
                    cpp("if (!msg.{}.has_value)", field_name);
                    cpp("{{");
                    cpp("throw std::runtime_error(\"Malformed nanopb optional field {}: wrapper is present without "
                        "value\");",
                        field_name);
                    cpp("}}");
                }
                cpp("{} {}_value {{}};", normalize_type(inner_type), field_name);
                write_nanopb_field_to_cpp_value(
                    lib, package_name, "value", inner_type, "msg." + field_name, field_name + "_value", field_name + "_value", cpp);
                cpp("{} = {}_value;", member_name, field_name);
                cpp("}}");
                cpp("else");
                cpp("{{");
                cpp("{}.reset();", member_name);
                cpp("}}");
            }
            else if (is_pointer_type(member_type))
            {
                cpp("{} = reinterpret_cast<{}>(static_cast<std::uintptr_t>(msg.{}));",
                    member_name,
                    pointer_cast_type(member_type),
                    field_name);
            }
            else if (is_enum_type(lib, member_type))
            {
                cpp("{} = static_cast<{}>(static_cast<std::underlying_type_t<{}>>(msg.{}));",
                    member_name,
                    enum_type_ref(lib, member_type),
                    enum_type_ref(lib, member_type),
                    field_name);
            }
            else if (is_error_type(lib, member_type))
            {
                cpp("{} = static_cast<int>(msg.{});", member_name, field_name);
            }
            else if (is_scalar_type(member_type))
            {
                cpp("{} = static_cast<{}>(msg.{});", member_name, normalize_type(member_type), field_name);
            }
            else
            {
                write_unsupported_member(cpp);
            }
        }
        cpp("}}");
        cpp("");
    }

    void write_struct_methods(
        const class_entity& lib,
        const class_entity& struct_entity,
        const std::string& package_name,
        writer& cpp)
    {
        if (struct_entity.get_is_template())
            return;

        const auto struct_name = struct_entity.get_name();
        const auto message_name = concrete_message_name(struct_entity);
        const auto c_type = nanopb_c_prefix(package_name) + "_" + message_name;

        cpp("void {}::nanopb_serialise(std::vector<char>& buffer) const", struct_name);
        cpp("{{");
        cpp("{} msg = {}_init_zero;", c_type, c_type);

        for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = field->get_name();
            const auto field_name = proto_generator::sanitize_field_name(member_name);
            const auto member_type = field->get_return_type();

            write_cpp_to_nanopb_field(lib, package_name, field_name, member_type, "(*this)", "msg", field_name, cpp);
        }

        cpp("rpc::serialization::nanopb::encode_message(buffer, {}_fields, &msg);", c_type);
        cpp("}}");
        cpp("");

        cpp("void {}::nanopb_deserialise(const std::vector<char>& buffer)", struct_name);
        cpp("{{");
        cpp("{} msg = {}_init_zero;", c_type, c_type);

        for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = field->get_name();
            const auto field_name = proto_generator::sanitize_field_name(member_name);
            const auto member_type = field->get_return_type();

            if (is_optional_type(member_type))
            {
                write_prepare_nanopb_field_decode(
                    lib, package_name, "value", optional_inner_type(member_type), "msg." + field_name, field_name + "_value", cpp);
            }
            else if (std::vector<std::string> variant_alternatives; is_variant_type(member_type, variant_alternatives))
            {
                write_prepare_nanopb_variant_decode(lib, package_name, member_type, "msg." + field_name, field_name, cpp);
            }
            else if (is_string_type(member_type))
            {
                cpp("std::string {}_decoded;", field_name);
                cpp("rpc::serialization::nanopb::string_decode_state {}_state {{ &{}_decoded }};", field_name, field_name);
                cpp("msg.{}.funcs.decode = rpc::serialization::nanopb::decode_string;", field_name);
                cpp("msg.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_byte_vector_type(member_type))
            {
                cpp("std::vector<uint8_t> {}_decoded;", field_name);
                cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};", field_name, field_name);
                cpp("msg.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", field_name);
                cpp("msg.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_json_dom_type(member_type))
            {
                cpp("std::vector<uint8_t> {}_decoded;", field_name);
                cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};", field_name, field_name);
                cpp("msg.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", field_name);
                cpp("msg.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_repeated_varint_type(member_type))
            {
                const auto inner_type = vector_inner_type(member_type);
                cpp("{} {}_decoded;", normalize_type(member_type), field_name);
                cpp("rpc::serialization::nanopb::repeated_varint_decode_state<{}> {}_state {{ &{}_decoded }};",
                    inner_type,
                    field_name,
                    field_name);
                cpp("msg.{}.funcs.decode = rpc::serialization::nanopb::decode_repeated_varint<{}>;", field_name, inner_type);
                cpp("msg.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_sequence_structure_type(member_type))
            {
                const auto inner_type = vector_inner_type(member_type);
                cpp("{} {}_decoded;", normalize_type(member_type), field_name);
                cpp("rpc::serialization::nanopb::sequence_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                    inner_type,
                    field_name,
                    field_name);
                cpp("msg.{}.funcs.decode = rpc::serialization::nanopb::decode_sequence_structure<{}>;",
                    field_name,
                    inner_type);
                cpp("msg.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_dictionary_structure_type(member_type))
            {
                cpp("{} {}_decoded;", normalize_type(member_type), field_name);
                cpp("rpc::serialization::nanopb::dictionary_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                    normalize_type(member_type),
                    field_name,
                    field_name);
                cpp("msg.{}.funcs.decode = rpc::serialization::nanopb::decode_dictionary_structure<{}>;",
                    field_name,
                    normalize_type(member_type));
                cpp("msg.{}.arg = &{}_state;", field_name, field_name);
            }
            else
            {
                std::string nested_concrete_template_param;
                const auto* nested_struct
                    = find_struct_or_template_entity(lib, member_type, &nested_concrete_template_param);
                if (nested_struct && !has_explicit_access_markers(*nested_struct))
                {
                    write_prepare_nanopb_message_decode(
                        lib, *nested_struct, package_name, "msg." + field_name, field_name, cpp, nested_concrete_template_param);
                }
            }
        }

        cpp("rpc::serialization::nanopb::decode_message(buffer, {}_fields, &msg);", c_type);

        for (const auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                continue;

            auto field = std::static_pointer_cast<function_entity>(member);
            if (field->is_static())
                continue;

            const auto member_name = field->get_name();
            const auto field_name = proto_generator::sanitize_field_name(member_name);
            const auto member_type = field->get_return_type();

            if (is_optional_type(member_type))
            {
                const auto inner_type = optional_inner_type(member_type);
                cpp("if (msg.has_{})", field_name);
                cpp("{{");
                if (optional_wrapper_value_has_nanopb_presence(lib, inner_type))
                {
                    cpp("if (!msg.{}.has_value)", field_name);
                    cpp("{{");
                    cpp("throw std::runtime_error(\"Malformed nanopb optional field {}: wrapper is present without "
                        "value\");",
                        field_name);
                    cpp("}}");
                }
                cpp("{} {}_value {{}};", normalize_type(inner_type), field_name);
                write_nanopb_field_to_cpp_value(
                    lib, package_name, "value", inner_type, "msg." + field_name, field_name + "_value", field_name + "_value", cpp);
                cpp("{} = {}_value;", member_name, field_name);
                cpp("}}");
                cpp("else");
                cpp("{{");
                cpp("{}.reset();", member_name);
                cpp("}}");
            }
            else if (std::vector<std::string> variant_alternatives; is_variant_type(member_type, variant_alternatives))
            {
                cpp("if (msg.has_{})", field_name);
                cpp("{{");
                write_nanopb_variant_to_cpp_value(
                    lib, package_name, member_type, "msg." + field_name, member_name, field_name, cpp);
                cpp("}}");
                cpp("else");
                cpp("{{");
                cpp("{} = {}{{}};", member_name, normalize_type(member_type));
                cpp("}}");
            }
            else if (is_pointer_type(member_type))
            {
                cpp("{} = reinterpret_cast<{}>(static_cast<std::uintptr_t>(msg.{}));",
                    member_name,
                    pointer_cast_type(member_type),
                    field_name);
            }
            else if (is_enum_type(lib, member_type))
            {
                cpp("{} = static_cast<{}>(static_cast<std::underlying_type_t<{}>>(msg.{}));",
                    member_name,
                    enum_type_ref(lib, member_type),
                    enum_type_ref(lib, member_type),
                    field_name);
            }
            else if (is_error_type(lib, member_type))
            {
                cpp("{} = static_cast<int>(msg.{});", member_name, field_name);
            }
            else if (is_scalar_type(member_type))
            {
                cpp("{} = static_cast<{}>(msg.{});", member_name, normalize_type(member_type), field_name);
            }
            else if (is_string_type(member_type))
            {
                cpp("{} = std::move({}_decoded);", member_name, field_name);
            }
            else if (is_byte_vector_type(member_type))
            {
                if (is_unsigned_byte_vector_type(member_type))
                    cpp("{} = std::move({}_decoded);", member_name, field_name);
                else
                    cpp("{}.assign({}_decoded.begin(), {}_decoded.end());", member_name, field_name, field_name);
            }
            else if (is_json_dom_type(member_type))
            {
                cpp("std::vector<char> {}_buffer({}_decoded.begin(), {}_decoded.end());", field_name, field_name, field_name);
                cpp("{}.nanopb_deserialise({}_buffer);", member_name, field_name);
            }
            else if (is_repeated_varint_type(member_type))
            {
                cpp("{} = std::move({}_decoded);", member_name, field_name);
            }
            else if (is_sequence_structure_type(member_type) || is_dictionary_structure_type(member_type))
            {
                cpp("{} = std::move({}_decoded);", member_name, field_name);
            }
            else if (is_unsupported_nanopb_field_type(member_type))
            {
                write_unsupported_member(cpp);
            }
            else
            {
                std::string nested_concrete_template_param;
                const auto* nested_struct
                    = find_struct_or_template_entity(lib, member_type, &nested_concrete_template_param);
                if (nested_struct && !has_explicit_access_markers(*nested_struct))
                {
                    write_nanopb_message_to_cpp(
                        lib,
                        *nested_struct,
                        package_name,
                        "msg." + field_name,
                        member_name,
                        field_name,
                        cpp,
                        nested_concrete_template_param);
                }
                else
                {
                    const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, member_type);
                    cpp("{{");
                    cpp("std::vector<char> {}_buffer;", field_name);
                    cpp("rpc::serialization::nanopb::encode_message({}_buffer, {}_fields, &msg.{});",
                        field_name,
                        nested_c_type,
                        field_name);
                    cpp("{}.nanopb_deserialise({}_buffer);", member_name, field_name);
                    cpp("}}");
                }
            }
        }

        cpp("}}");
        cpp("");
    }

    void append_signature_param(
        std::string& signature,
        bool& first_param,
        const std::string& type,
        const std::string& name)
    {
        if (!first_param)
            signature += ", ";
        first_param = false;
        signature += type + " " + name;
    }

    void write_prepare_decode_callbacks(
        const class_entity& lib,
        const std::string& package_name,
        const std::vector<const parameter_entity*>& params,
        writer& cpp)
    {
        for (const auto* parameter : params)
        {
            const auto field_name = proto_generator::sanitize_field_name(parameter->get_name());
            const auto param_type = parameter->get_type();
            if (is_optional_type(param_type))
            {
                write_prepare_nanopb_field_decode(
                    lib,
                    package_name,
                    "value",
                    optional_inner_type(param_type),
                    "__message." + field_name,
                    field_name + "_value",
                    cpp);
            }
            else if (std::vector<std::string> variant_alternatives; is_variant_type(param_type, variant_alternatives))
            {
                write_prepare_nanopb_variant_decode(
                    lib, package_name, param_type, "__message." + field_name, field_name, cpp);
            }
            else if (is_interface_type(param_type))
            {
                cpp("std::vector<uint8_t> {}_addr_decoded;", field_name);
                cpp("rpc::serialization::nanopb::bytes_decode_state {}_addr_state {{ &{}_addr_decoded }};",
                    field_name,
                    field_name);
                cpp("__message.{}.addr_.blob.funcs.decode = rpc::serialization::nanopb::decode_bytes;", field_name);
                cpp("__message.{}.addr_.blob.arg = &{}_addr_state;", field_name, field_name);
            }
            else if (is_string_type(param_type))
            {
                cpp("std::string {}_decoded;", field_name);
                cpp("rpc::serialization::nanopb::string_decode_state {}_state {{ &{}_decoded }};", field_name, field_name);
                cpp("__message.{}.funcs.decode = rpc::serialization::nanopb::decode_string;", field_name);
                cpp("__message.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_byte_vector_type(param_type))
            {
                if (is_unsigned_byte_vector_type(param_type))
                {
                    cpp("{}.clear();", parameter->get_name());
                    cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{} }};",
                        field_name,
                        parameter->get_name());
                }
                else
                {
                    cpp("std::vector<uint8_t> {}_decoded;", field_name);
                    cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};", field_name, field_name);
                }
                cpp("__message.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", field_name);
                cpp("__message.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_json_dom_type(param_type))
            {
                cpp("std::vector<uint8_t> {}_decoded;", field_name);
                cpp("rpc::serialization::nanopb::bytes_decode_state {}_state {{ &{}_decoded }};", field_name, field_name);
                cpp("__message.{}.funcs.decode = rpc::serialization::nanopb::decode_bytes;", field_name);
                cpp("__message.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_repeated_varint_type(param_type))
            {
                const auto inner_type = vector_inner_type(param_type);
                cpp("{} {}_decoded;", normalize_type(param_type), field_name);
                cpp("rpc::serialization::nanopb::repeated_varint_decode_state<{}> {}_state {{ &{}_decoded }};",
                    inner_type,
                    field_name,
                    field_name);
                cpp("__message.{}.funcs.decode = rpc::serialization::nanopb::decode_repeated_varint<{}>;",
                    field_name,
                    inner_type);
                cpp("__message.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_sequence_structure_type(param_type))
            {
                const auto inner_type = vector_inner_type(param_type);
                cpp("{} {}_decoded;", normalize_type(param_type), field_name);
                cpp("rpc::serialization::nanopb::sequence_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                    inner_type,
                    field_name,
                    field_name);
                cpp("__message.{}.funcs.decode = rpc::serialization::nanopb::decode_sequence_structure<{}>;",
                    field_name,
                    inner_type);
                cpp("__message.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (is_dictionary_structure_type(param_type))
            {
                cpp("{} {}_decoded;", normalize_type(param_type), field_name);
                cpp("rpc::serialization::nanopb::dictionary_structure_decode_state<{}> {}_state {{ &{}_decoded }};",
                    normalize_type(param_type),
                    field_name,
                    field_name);
                cpp("__message.{}.funcs.decode = rpc::serialization::nanopb::decode_dictionary_structure<{}>;",
                    field_name,
                    normalize_type(param_type));
                cpp("__message.{}.arg = &{}_state;", field_name, field_name);
            }
            else if (!is_pointer_type(param_type) && !is_interface_type(param_type) && !is_scalar_type(param_type)
                     && !is_enum_type(lib, param_type) && !is_error_type(lib, param_type))
            {
                std::string nested_concrete_template_param;
                if (const auto* nested_struct
                    = find_struct_or_template_entity(lib, param_type, &nested_concrete_template_param))
                {
                    write_prepare_nanopb_message_decode(
                        lib, *nested_struct, package_name, "__message." + field_name, field_name, cpp, nested_concrete_template_param);
                }
            }
        }
    }

    void write_encode_field(
        const class_entity& lib,
        const std::string& package_name,
        const parameter_entity& parameter,
        const std::string& source_name,
        writer& cpp)
    {
        const auto field_name = proto_generator::sanitize_field_name(parameter.get_name());
        const auto param_type = parameter.get_type();

        if (is_optional_type(param_type))
        {
            const auto inner_type = optional_inner_type(param_type);
            write_optional_nanopb_encode_state_storage(inner_type, field_name + "_value", cpp);
            cpp("if ({}.has_value())", source_name);
            cpp("{{");
            write_optional_cpp_value_to_nanopb_field(
                lib,
                package_name,
                "value",
                inner_type,
                source_name + ".value()",
                "__message." + field_name,
                field_name + "_value",
                cpp);
            cpp("__message.has_{} = true;", field_name);
            cpp("}}");
            cpp("else");
            cpp("{{");
            cpp("__message.has_{} = false;", field_name);
            cpp("}}");
        }
        else if (std::vector<std::string> variant_alternatives; is_variant_type(param_type, variant_alternatives))
        {
            write_cpp_variant_to_nanopb_field(
                lib, package_name, field_name, param_type, source_name, "__message", field_name, cpp);
        }
        else if (is_enum_type(lib, param_type))
        {
            cpp("__message.{} = static_cast<decltype(__message.{})>(static_cast<std::underlying_type_t<{}>>({}));",
                field_name,
                field_name,
                enum_type_ref(lib, param_type),
                source_name);
        }
        else if (is_error_type(lib, param_type))
        {
            cpp("__message.{} = static_cast<decltype(__message.{})>(static_cast<int>({}));", field_name, field_name, source_name);
        }
        else if (is_pointer_type(param_type) || is_scalar_type(param_type))
        {
            cpp("__message.{} = static_cast<decltype(__message.{})>({});", field_name, field_name, source_name);
        }
        else if (is_interface_type(param_type))
        {
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_addr_state;", field_name);
            cpp("if ({}.is_set())", source_name);
            cpp("{{");
            cpp("const auto& {}_addr_blob = {}.get_address().get_blob();", field_name, source_name);
            cpp("{}_addr_state = rpc::serialization::nanopb::bytes_encode_state {{ {}_addr_blob.data(), "
                "{}_addr_blob.size() }};",
                field_name,
                field_name,
                field_name);
            cpp("__message.{}.has_addr_ = true;", field_name);
            cpp("__message.{}.addr_.blob.funcs.encode = rpc::serialization::nanopb::encode_bytes;", field_name);
            cpp("__message.{}.addr_.blob.arg = &{}_addr_state;", field_name, field_name);
            cpp("__message.has_{} = true;", field_name);
            cpp("}}");
        }
        else if (is_string_type(param_type))
        {
            cpp("rpc::serialization::nanopb::string_encode_state {}_state {{ &{} }};", field_name, source_name);
            cpp("__message.{}.funcs.encode = rpc::serialization::nanopb::encode_string;", field_name);
            cpp("__message.{}.arg = &{}_state;", field_name, field_name);
        }
        else if (is_byte_vector_type(param_type))
        {
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state {{ {}.data(), {}.size() }};",
                field_name,
                source_name,
                source_name);
            cpp("__message.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", field_name);
            cpp("__message.{}.arg = &{}_state;", field_name, field_name);
        }
        else if (is_json_dom_type(param_type))
        {
            cpp("std::vector<char> {}_buffer;", field_name);
            cpp("{}.nanopb_serialise({}_buffer);", source_name, field_name);
            cpp("rpc::serialization::nanopb::bytes_encode_state {}_state {{ {}_buffer.data(), {}_buffer.size() }};",
                field_name,
                field_name,
                field_name);
            cpp("__message.{}.funcs.encode = rpc::serialization::nanopb::encode_bytes;", field_name);
            cpp("__message.{}.arg = &{}_state;", field_name, field_name);
        }
        else if (is_repeated_varint_type(param_type))
        {
            const auto inner_type = vector_inner_type(param_type);
            cpp("rpc::serialization::nanopb::repeated_varint_encode_state<{}> {}_state {{ &{} }};",
                inner_type,
                field_name,
                source_name);
            cpp("__message.{}.funcs.encode = rpc::serialization::nanopb::encode_repeated_varint<{}>;", field_name, inner_type);
            cpp("__message.{}.arg = &{}_state;", field_name, field_name);
        }
        else if (is_sequence_structure_type(param_type))
        {
            const auto inner_type = vector_inner_type(param_type);
            cpp("rpc::serialization::nanopb::sequence_structure_encode_state<{}> {}_state {{ &{} }};",
                inner_type,
                field_name,
                source_name);
            cpp("__message.{}.funcs.encode = rpc::serialization::nanopb::encode_sequence_structure<{}>;",
                field_name,
                inner_type);
            cpp("__message.{}.arg = &{}_state;", field_name, field_name);
        }
        else if (is_dictionary_structure_type(param_type))
        {
            cpp("rpc::serialization::nanopb::dictionary_structure_encode_state<{}> {}_state {{ &{} }};",
                normalize_type(param_type),
                field_name,
                source_name);
            cpp("__message.{}.funcs.encode = rpc::serialization::nanopb::encode_dictionary_structure<{}>;",
                field_name,
                normalize_type(param_type));
            cpp("__message.{}.arg = &{}_state;", field_name, field_name);
        }
        else if (is_unsupported_nanopb_field_type(param_type))
        {
            cpp("(void){};", source_name);
            cpp("throw std::runtime_error(\"Nanopb serialization is not implemented for this IDL parameter "
                "category\");");
        }
        else
        {
            std::string nested_concrete_template_param;
            if (const auto* nested_struct
                = find_struct_or_template_entity(lib, param_type, &nested_concrete_template_param))
            {
                write_cpp_to_nanopb_message(
                    lib,
                    *nested_struct,
                    package_name,
                    source_name,
                    "__message." + field_name,
                    field_name,
                    cpp,
                    nested_concrete_template_param);
                cpp("__message.has_{} = true;", field_name);
            }
            else
            {
                const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, param_type);
                cpp("{{");
                cpp("std::vector<char> {}_buffer;", field_name);
                cpp("{}.nanopb_serialise({}_buffer);", source_name, field_name);
                cpp("rpc::serialization::nanopb::decode_message({}_buffer, {}_fields, &__message.{});",
                    field_name,
                    nested_c_type,
                    field_name);
                cpp("__message.has_{} = true;", field_name);
                cpp("}}");
            }
        }
    }

    void write_decode_field(
        const class_entity& lib,
        const std::string& package_name,
        const parameter_entity& parameter,
        const std::string& destination_name,
        writer& cpp)
    {
        const auto field_name = proto_generator::sanitize_field_name(parameter.get_name());
        const auto param_type = parameter.get_type();

        if (is_optional_type(param_type))
        {
            const auto inner_type = optional_inner_type(param_type);
            cpp("if (__message.has_{})", field_name);
            cpp("{{");
            if (optional_wrapper_value_has_nanopb_presence(lib, inner_type))
            {
                cpp("if (!__message.{}.has_value)", field_name);
                cpp("{{");
                cpp("throw std::runtime_error(\"Malformed nanopb optional field {}: wrapper is present without "
                    "value\");",
                    field_name);
                cpp("}}");
            }
            cpp("{} {}_value {{}};", normalize_type(inner_type), field_name);
            write_nanopb_field_to_cpp_value(
                lib, package_name, "value", inner_type, "__message." + field_name, field_name + "_value", field_name + "_value", cpp);
            cpp("{} = {}_value;", destination_name, field_name);
            cpp("}}");
            cpp("else");
            cpp("{{");
            cpp("{}.reset();", destination_name);
            cpp("}}");
        }
        else if (std::vector<std::string> variant_alternatives; is_variant_type(param_type, variant_alternatives))
        {
            cpp("if (__message.has_{})", field_name);
            cpp("{{");
            write_nanopb_variant_to_cpp_value(
                lib, package_name, param_type, "__message." + field_name, destination_name, field_name, cpp);
            cpp("}}");
            cpp("else");
            cpp("{{");
            cpp("{} = {}{{}};", destination_name, normalize_type(param_type));
            cpp("}}");
        }
        else if (is_pointer_type(param_type))
        {
            cpp("{} = __message.{};", destination_name, field_name);
        }
        else if (is_enum_type(lib, param_type))
        {
            cpp("{} = static_cast<{}>(static_cast<std::underlying_type_t<{}>>(__message.{}));",
                destination_name,
                enum_type_ref(lib, param_type),
                enum_type_ref(lib, param_type),
                field_name);
        }
        else if (is_error_type(lib, param_type))
        {
            cpp("{} = static_cast<int>(__message.{});", destination_name, field_name);
        }
        else if (is_scalar_type(param_type))
        {
            cpp("{} = static_cast<{}>(__message.{});", destination_name, normalize_type(param_type), field_name);
        }
        else if (is_interface_type(param_type))
        {
            cpp("if (__message.has_{})", field_name);
            cpp("{{");
            cpp("auto {}_addr = rpc::zone_address::from_blob(std::move({}_addr_decoded));", field_name, field_name);
            cpp("if (!{}_addr)", field_name);
            cpp("throw std::runtime_error(\"Invalid Nanopb remote object address\");");
            cpp("{} = rpc::remote_object(*{}_addr);", destination_name, field_name);
            cpp("}}");
            cpp("else");
            cpp("{{");
            cpp("{} = rpc::remote_object();", destination_name);
            cpp("}}");
        }
        else if (is_string_type(param_type))
        {
            cpp("{} = std::move({}_decoded);", destination_name, field_name);
        }
        else if (is_byte_vector_type(param_type))
        {
            if (!is_unsigned_byte_vector_type(param_type))
                cpp("{}.assign({}_decoded.begin(), {}_decoded.end());", destination_name, field_name, field_name);
        }
        else if (is_json_dom_type(param_type))
        {
            cpp("std::vector<char> {}_buffer({}_decoded.begin(), {}_decoded.end());", field_name, field_name, field_name);
            cpp("{}.nanopb_deserialise({}_buffer);", destination_name, field_name);
        }
        else if (is_repeated_varint_type(param_type))
        {
            cpp("{} = std::move({}_decoded);", destination_name, field_name);
        }
        else if (is_sequence_structure_type(param_type) || is_dictionary_structure_type(param_type))
        {
            cpp("{} = std::move({}_decoded);", destination_name, field_name);
        }
        else if (is_unsupported_nanopb_field_type(param_type))
        {
            cpp("(void){};", destination_name);
            cpp("throw std::runtime_error(\"Nanopb deserialization is not implemented for this IDL parameter "
                "category\");");
        }
        else
        {
            std::string nested_concrete_template_param;
            if (const auto* nested_struct
                = find_struct_or_template_entity(lib, param_type, &nested_concrete_template_param))
            {
                write_nanopb_message_to_cpp(
                    lib,
                    *nested_struct,
                    package_name,
                    "__message." + field_name,
                    destination_name,
                    field_name,
                    cpp,
                    nested_concrete_template_param);
            }
            else
            {
                const auto nested_c_type = nanopb_c_type_for_cpp_type(package_name, param_type);
                cpp("{{");
                cpp("std::vector<char> {}_buffer;", field_name);
                cpp("rpc::serialization::nanopb::encode_message({}_buffer, {}_fields, &__message.{});",
                    field_name,
                    nested_c_type,
                    field_name);
                cpp("{}.nanopb_deserialise({}_buffer);", destination_name, field_name);
                cpp("}}");
            }
        }
    }

    void write_unsupported_interface_method_body(
        writer& cpp,
        const std::vector<std::string>& param_names,
        const std::string& buffer_name,
        const std::string& error_call)
    {
        for (const auto& param_name : param_names)
            cpp("(void){};", param_name);
        if (!buffer_name.empty())
            cpp("(void){};", buffer_name);
        cpp("return {};", error_call);
    }

    void write_interface_methods(
        const class_entity& lib,
        const class_entity& interface_entity,
        const std::string& package_name,
        writer& cpp)
    {
        const auto interface_name = interface_entity.get_name();

        for (const auto& function : interface_entity.get_functions())
        {
            if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                continue;

            const auto function_name = function->get_name();

            std::vector<const parameter_entity*> in_params;
            std::vector<const parameter_entity*> out_params;
            for (const auto& parameter : function->get_parameters())
            {
                if (is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter)))
                    in_params.push_back(&parameter);
                if (is_out_param(parameter))
                    out_params.push_back(&parameter);
            }

            for (const std::string serialiser_name : {"protocol_buffers", "nanopb"})
            {
                const bool is_protocol_buffers_replacement = serialiser_name == "protocol_buffers";
                if (is_protocol_buffers_replacement)
                    cpp("#ifdef CANOPY_USE_NANOPB_FOR_PROTOCOL_BUFFERS");

                cpp("template<>");
                cpp("int {}::proxy_serialiser<rpc::serialiser::{}>::{}(", interface_name, serialiser_name, function_name);
                std::vector<std::string> param_names;
                for (const auto* parameter : in_params)
                {
                    param_names.push_back(parameter->get_name());
                    cpp("{} {},", proxy_input_type(parameter->get_type()), parameter->get_name());
                }
                cpp("std::vector<char>& __buffer)");
                cpp("{{");
                const auto request_c_type
                    = nanopb_c_prefix(package_name) + "_" + interface_name + "_" + function_name + "Request";
                cpp("try");
                cpp("{{");
                cpp("{} __message = {}_init_zero;", request_c_type, request_c_type);
                for (const auto* parameter : in_params)
                    write_encode_field(lib, package_name, *parameter, parameter->get_name(), cpp);
                cpp("rpc::serialization::nanopb::encode_message(__buffer, {}_fields, &__message);", request_c_type);
                cpp("return rpc::error::OK();");
                cpp("}}");
                cpp("catch (const std::exception&)");
                cpp("{{");
                cpp("__buffer.clear();");
                cpp("return rpc::error::PROXY_DESERIALISATION_ERROR();");
                cpp("}}");
                cpp("}}");
                cpp("");

                cpp("template<>");
                std::string proxy_deserialiser_signature = "int " + interface_name
                                                           + "::proxy_deserialiser<rpc::serialiser::" + serialiser_name
                                                           + ">::" + function_name + "(";
                bool first_param = true;
                param_names.clear();
                for (const auto* parameter : out_params)
                {
                    param_names.push_back(parameter->get_name());
                    append_signature_param(
                        proxy_deserialiser_signature,
                        first_param,
                        proxy_output_type(parameter->get_type()),
                        parameter->get_name());
                }
                if (!first_param)
                    proxy_deserialiser_signature += ", ";
                proxy_deserialiser_signature += "const rpc::byte_span& __rpc_data)";
                cpp(proxy_deserialiser_signature);
                cpp("{{");
                const auto response_c_type
                    = nanopb_c_prefix(package_name) + "_" + interface_name + "_" + function_name + "Response";
                cpp("try");
                cpp("{{");
                cpp("{} __message = {}_init_zero;", response_c_type, response_c_type);
                write_prepare_decode_callbacks(lib, package_name, out_params, cpp);
                cpp("rpc::serialization::nanopb::decode_message(__rpc_data.data(), __rpc_data.size(), {}_fields, "
                    "&__message);",
                    response_c_type);
                for (const auto* parameter : out_params)
                    write_decode_field(lib, package_name, *parameter, parameter->get_name(), cpp);
                cpp("return __message.result;");
                cpp("}}");
                cpp("catch (const std::exception&)");
                cpp("{{");
                cpp("return rpc::error::PROXY_DESERIALISATION_ERROR();");
                cpp("}}");
                cpp("}}");
                cpp("");

                cpp("template<>");
                std::string stub_deserialiser_signature = "int " + interface_name + "::stub_deserialiser<rpc::serialiser::"
                                                          + serialiser_name + ">::" + function_name + "(";
                first_param = true;
                param_names.clear();
                for (const auto* parameter : in_params)
                {
                    param_names.push_back(parameter->get_name());
                    append_signature_param(
                        stub_deserialiser_signature,
                        first_param,
                        stub_input_type(parameter->get_type()),
                        parameter->get_name());
                }
                if (!first_param)
                    stub_deserialiser_signature += ", ";
                stub_deserialiser_signature += "const rpc::byte_span& __rpc_data)";
                cpp(stub_deserialiser_signature);
                cpp("{{");
                const auto stub_request_c_type
                    = nanopb_c_prefix(package_name) + "_" + interface_name + "_" + function_name + "Request";
                cpp("try");
                cpp("{{");
                cpp("{} __message = {}_init_zero;", stub_request_c_type, stub_request_c_type);
                write_prepare_decode_callbacks(lib, package_name, in_params, cpp);
                cpp("rpc::serialization::nanopb::decode_message(__rpc_data.data(), __rpc_data.size(), {}_fields, "
                    "&__message);",
                    stub_request_c_type);
                for (const auto* parameter : in_params)
                    write_decode_field(lib, package_name, *parameter, parameter->get_name(), cpp);
                cpp("return rpc::error::OK();");
                cpp("}}");
                cpp("catch (const std::exception&)");
                cpp("{{");
                cpp("return rpc::error::STUB_DESERIALISATION_ERROR();");
                cpp("}}");
                cpp("}}");
                cpp("");

                cpp("template<>");
                std::string stub_serialiser_signature = "int " + interface_name + "::stub_serialiser<rpc::serialiser::"
                                                        + serialiser_name + ">::" + function_name + "(";
                first_param = true;
                param_names.clear();
                for (const auto* parameter : out_params)
                {
                    param_names.push_back(parameter->get_name());
                    append_signature_param(
                        stub_serialiser_signature,
                        first_param,
                        stub_output_type(parameter->get_type()),
                        parameter->get_name());
                }
                if (!first_param)
                    stub_serialiser_signature += ", ";
                stub_serialiser_signature += "std::vector<char>& __buffer)";
                cpp(stub_serialiser_signature);
                cpp("{{");
                const auto stub_response_c_type
                    = nanopb_c_prefix(package_name) + "_" + interface_name + "_" + function_name + "Response";
                cpp("try");
                cpp("{{");
                cpp("{} __message = {}_init_zero;", stub_response_c_type, stub_response_c_type);
                for (const auto* parameter : out_params)
                    write_encode_field(lib, package_name, *parameter, parameter->get_name(), cpp);
                cpp("__message.result = rpc::error::OK();");
                cpp("rpc::serialization::nanopb::encode_message(__buffer, {}_fields, &__message);", stub_response_c_type);
                cpp("return rpc::error::OK();");
                cpp("}}");
                cpp("catch (const std::exception&)");
                cpp("{{");
                cpp("__buffer.clear();");
                cpp("return rpc::error::STUB_DESERIALISATION_ERROR();");
                cpp("}}");
                cpp("}}");
                cpp("");

                if (is_protocol_buffers_replacement)
                    cpp("#endif");
            }
        }
        cpp("");
    }

    void write_namespace_methods(
        const class_entity& entity,
        writer& cpp)
    {
        const auto cpp_namespace = entity.get_name();
        const bool has_cpp_namespace = !cpp_namespace.empty();
        if (has_cpp_namespace)
        {
            if (entity.has_value("inline"))
                cpp("inline namespace {} {{", cpp_namespace);
            else
                cpp("namespace {} {{", cpp_namespace);
        }

        const auto package_name = namespace_name(entity);
        std::set<TemplateInstantiation> template_instantiations;
        collect_template_instantiations(entity, template_instantiations);
        for (const auto& inst : template_instantiations)
        {
            for (const auto& elem : entity.get_elements(entity_type::STRUCT))
            {
                auto& struct_entity = static_cast<const class_entity&>(*elem);
                if (struct_entity.get_is_template() && struct_entity.get_name() == inst.template_name)
                    write_template_instantiation_methods(entity, struct_entity, inst, package_name, cpp);
            }
        }

        for (const auto& elem : entity.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->is_in_import())
                continue;

            if (elem->get_entity_type() == entity_type::NAMESPACE)
            {
                write_namespace_methods(static_cast<const class_entity&>(*elem), cpp);
            }
            else if (elem->get_entity_type() == entity_type::STRUCT)
            {
                write_struct_methods(entity, static_cast<const class_entity&>(*elem), package_name, cpp);
            }
            else if (elem->get_entity_type() == entity_type::INTERFACE)
            {
                write_interface_methods(entity, static_cast<const class_entity&>(*elem), package_name, cpp);
            }
        }

        if (has_cpp_namespace)
            cpp("}} // namespace {}", cpp_namespace);
    }

    void write_cpp_files(
        const class_entity& lib,
        std::ostream& cpp_stream,
        const std::vector<std::string>& namespaces,
        const std::filesystem::path& header_filename,
        const std::filesystem::path& nanopb_include_path,
        const std::vector<std::string>& additional_stub_headers)
    {
        writer cpp(cpp_stream);

        cpp("#include <rpc/rpc.h>");
        cpp("#include <rpc/serialization/nanopb/nanopb.h>");
        cpp("#include \"{}\"", header_filename.generic_string());
        cpp("#include \"{}\"", nanopb_include_path.generic_string());
        cpp("#include <cstdint>");
        cpp("#include <type_traits>");
        for (const auto& header : additional_stub_headers)
            cpp("#include \"{}\"", header);
        cpp("");

        (void)namespaces;
        write_namespace_methods(lib, cpp);
    }
}
