/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "rust_protobuf_generator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "coreclasses.h"
#include "helpers.h"
#include "proto_generator.h"
#include "writer.h"

namespace rust_protobuf_generator
{
    namespace
    {
        bool is_rust_keyword(const std::string& name)
        {
            static const std::unordered_set<std::string> keywords = {
                "as",
                "break",
                "const",
                "continue",
                "crate",
                "else",
                "enum",
                "extern",
                "false",
                "fn",
                "for",
                "if",
                "impl",
                "in",
                "let",
                "loop",
                "match",
                "mod",
                "move",
                "mut",
                "pub",
                "ref",
                "return",
                "self",
                "Self",
                "static",
                "struct",
                "super",
                "trait",
                "true",
                "type",
                "unsafe",
                "use",
                "where",
                "while",
                "async",
                "await",
                "dyn",
                "abstract",
                "become",
                "box",
                "do",
                "final",
                "macro",
                "override",
                "priv",
                "typeof",
                "unsized",
                "virtual",
                "yield",
                "try",
            };
            return keywords.find(name) != keywords.end();
        }

        std::string sanitize_identifier_base(const std::string& name)
        {
            std::string result;
            result.reserve(name.size() + 1);

            for (char ch : name)
            {
                if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')
                    result += ch;
                else
                    result += '_';
            }

            if (result.empty())
                result = "_";

            if (std::isdigit(static_cast<unsigned char>(result.front())))
                result.insert(result.begin(), '_');

            return result;
        }

        std::string sanitize_identifier(const std::string& name)
        {
            auto result = sanitize_identifier_base(name);
            if (is_rust_keyword(result))
                result = "r#" + result;

            return result;
        }

        std::string upper_camel_identifier(const std::string& name)
        {
            auto sanitized = sanitize_identifier_base(name);
            std::string result;
            result.reserve(sanitized.size());
            bool uppercase_next = true;
            for (char ch : sanitized)
            {
                if (ch == '_')
                {
                    uppercase_next = true;
                    continue;
                }
                if (uppercase_next)
                    result += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                else
                    result += ch;
                uppercase_next = false;
            }
            if (result.empty())
                result = "Type";
            return result;
        }

        std::string unique_identifier(
            std::string base,
            std::set<std::string>& used_identifiers)
        {
            if (used_identifiers.insert(base).second)
                return base;

            for (size_t index = 1;; ++index)
            {
                auto candidate = base + std::to_string(index);
                if (used_identifiers.insert(candidate).second)
                    return candidate;
            }
        }

        std::string qualified_rust_namespace_path(const class_entity& entity)
        {
            std::vector<std::string> parts;

            auto current = entity.get_owner();
            while (current != nullptr && !current->get_name().empty() && current->get_name() != "__global__")
            {
                parts.push_back(sanitize_identifier_base(current->get_name()));
                current = current->get_owner();
            }

            std::reverse(parts.begin(), parts.end());

            std::string result;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i)
                    result += "::";
                result += parts[i];
            }
            return result;
        }

        std::string qualified_public_type_path(const class_entity& entity)
        {
            auto result = qualified_rust_namespace_path(entity);
            if (!result.empty())
                result += "::";
            result += upper_camel_identifier(entity.get_name());
            return result;
        }

        std::string qualified_generated_interface_path(const class_entity& iface)
        {
            auto result = qualified_rust_namespace_path(iface);
            if (!result.empty())
                result += "::";
            result += "__Generated::" + upper_camel_identifier(iface.get_name());
            return result;
        }

        bool is_enum_type(
            const class_entity& lib,
            const std::string& type_name)
        {
            return proto_generator::is_enum_type(lib, type_name);
        }

        std::string cpp_type_to_proto_type(
            const class_entity& iface,
            const class_entity& lib,
            const std::string& cpp_type)
        {
            return proto_generator::cpp_type_to_proto_type(
                cpp_type,
                [&](const std::string& type_name)
                {
                    bool is_optimistic = false;
                    std::shared_ptr<class_entity> interface_entity;
                    return is_interface_param(iface, type_name, is_optimistic, interface_entity);
                },
                [&](const std::string& type_name) { return is_enum_type(lib, type_name); },
                [&](const std::string& type_name) { return sanitize_identifier_base(type_name); });
        }

        std::string protobuf_field_kind(
            const class_entity& iface,
            const class_entity& lib,
            const std::string& cpp_type)
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
            type = proto_generator::trim_copy(type);

            if (is_pointer)
                return "PointerAddress";

            if (type == "std::vector<uint8_t>" || type == "std::vector<unsigned char>" || type == "std::vector<char>"
                || type == "std::vector<signed char>")
                return "Bytes";

            std::string container_prefix;
            if (proto_generator::is_map_type(type, container_prefix))
                return "MapScalar";
            if (proto_generator::is_sequence_type(type, container_prefix))
                return "RepeatedScalar";

            bool is_optimistic = false;
            std::shared_ptr<class_entity> interface_entity;
            if (is_interface_param(iface, type, is_optimistic, interface_entity))
                return "InterfaceRemoteObject";

            if (!proto_generator::cpp_scalar_to_proto_type(type).empty())
                return "Scalar";

            if (is_enum_type(lib, type))
                return "Enum";

            return "Message";
        }

        const char* direction_name(
            bool is_in,
            bool is_out)
        {
            if (is_out)
                return "Out";
            if (is_in || !is_out)
                return "In";
            return "In";
        }

        struct rust_interface_generic
        {
            std::string param_name;
            std::string generic_name;
            std::string trait_path;
            bool is_in = false;
            bool is_out = false;
            bool is_optimistic = false;
        };

        std::vector<rust_interface_generic> analyse_interface_generics(
            const class_entity& iface,
            const function_entity& function,
            const std::string& root_module_name)
        {
            std::vector<rust_interface_generic> result;
            std::set<std::string> used_generic_names;

            for (const auto& parameter : function.get_parameters())
            {
                bool is_optimistic = false;
                std::shared_ptr<class_entity> interface_entity;
                if (!is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                    continue;

                rust_interface_generic generic;
                generic.param_name = sanitize_identifier(parameter.get_name());
                generic.generic_name
                    = unique_identifier(upper_camel_identifier(parameter.get_name()) + "Iface", used_generic_names);
                generic.trait_path = "crate::" + sanitize_identifier(root_module_name)
                                     + "::" + qualified_public_type_path(*interface_entity);
                generic.is_in = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                generic.is_out = is_out_param(parameter);
                generic.is_optimistic = is_optimistic;
                result.push_back(std::move(generic));
            }

            return result;
        }

        std::string generic_declaration_for_params(
            const std::vector<rust_interface_generic>& params,
            bool include_in,
            bool include_out)
        {
            std::vector<std::string> generics;
            for (const auto& param : params)
            {
                if ((param.is_in && include_in) || (param.is_out && include_out))
                    generics.push_back(param.generic_name);
            }

            if (generics.empty())
                return "";

            std::string result = "<";
            for (size_t i = 0; i < generics.size(); ++i)
            {
                if (i)
                    result += ", ";
                result += generics[i];
            }
            result += ">";
            return result;
        }

        std::string generic_where_clause_for_params(
            const std::vector<rust_interface_generic>& params,
            bool include_in,
            bool include_out)
        {
            std::vector<std::string> constraints;
            for (const auto& param : params)
            {
                if ((param.is_in && include_in) || (param.is_out && include_out))
                {
                    constraints.push_back(param.generic_name + ": " + param.trait_path);
                }
            }

            if (constraints.empty())
                return "";

            std::string result = " where ";
            for (size_t i = 0; i < constraints.size(); ++i)
            {
                if (i)
                    result += ", ";
                result += constraints[i];
            }
            return result;
        }

        std::string phantom_return_type_for_generics(const std::string& generics)
        {
            if (generics.empty())
                return "()";
            return "(" + generics.substr(1, generics.size() - 2) + ",)";
        }

        std::string protobuf_namespace_path(const class_entity& entity)
        {
            std::vector<std::string> parts;

            auto current = entity.get_owner();
            while (current != nullptr && !current->get_name().empty() && current->get_name() != "__global__")
            {
                parts.push_back(sanitize_identifier_base(current->get_name()));
                current = current->get_owner();
            }

            std::reverse(parts.begin(), parts.end());

            std::string result;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i)
                    result += ".";
                result += parts[i];
            }
            return result;
        }

        std::string protobuf_package_for_interface(const class_entity& iface)
        {
            auto package = std::string("protobuf");
            const auto suffix = protobuf_namespace_path(iface);
            if (!suffix.empty())
                package += "." + suffix;
            return package;
        }

        std::string protobuf_schema_file_for_interface(const class_entity& iface)
        {
            auto path = protobuf_namespace_path(iface);
            if (path.empty())
                path = "default";
            std::replace(path.begin(), path.end(), '.', '/');
            return path + ".proto";
        }

        std::vector<std::string> generated_rs_candidates(
            const std::vector<std::string>& generated_proto_files,
            const std::string& master_proto)
        {
            std::vector<std::string> result;
            result.reserve((generated_proto_files.size() + 1) * 2);

            for (const auto& proto_file : generated_proto_files)
            {
                auto proto_path = std::filesystem::path(proto_file);
                const auto stem = proto_path.stem().string();
                const auto parent = proto_path.parent_path();
                result.push_back((parent / (stem + ".c.pb.rs")).generic_string());
                result.push_back((parent / (stem + ".u.pb.rs")).generic_string());
            }

            auto master_proto_path = std::filesystem::path(master_proto);
            const auto master_stem = master_proto_path.stem().string();
            const auto master_parent = master_proto_path.parent_path();
            result.push_back((master_parent / (master_stem + ".c.pb.rs")).generic_string());
            result.push_back((master_parent / (master_stem + ".u.pb.rs")).generic_string());

            return result;
        }

        std::string normalise_cpp_type(std::string type_name)
        {
            std::string reference_modifiers;
            strip_reference_modifiers(type_name, reference_modifiers);
            type_name = proto_generator::trim_copy(type_name);

            if (type_name.rfind("const ", 0) == 0)
                type_name = proto_generator::trim_copy(type_name.substr(6));

            return type_name;
        }

        bool extract_vector_value_type(
            const std::string& cpp_type,
            std::string& inner_type)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            std::string container_prefix;
            if (!proto_generator::is_sequence_type(type_name, container_prefix) || container_prefix != "std::vector<")
                return false;

            std::string template_content;
            const auto end_pos
                = proto_generator::extract_template_content(type_name, container_prefix.size() - 1, template_content);
            if (end_pos == std::string::npos || end_pos != type_name.size())
                return false;

            inner_type = normalise_cpp_type(template_content);
            return !inner_type.empty();
        }

        bool is_supported_scalar_codegen_type(const std::string& cpp_type)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            return type_name == "error_code" || type_name == "bool" || type_name == "int" || type_name == "uint64_t"
                   || type_name == "size_t" || type_name == "std::string" || type_name == "std::vector<uint8_t>"
                   || type_name == "std::vector<unsigned char>" || type_name == "std::vector<char>"
                   || type_name == "std::vector<signed char>";
        }

        std::string qualified_cpp_type_name(const class_entity& entity)
        {
            return get_full_name(entity, true, false, "::");
        }

        std::string protobuf_struct_helper_name(const class_entity& entity)
        {
            return sanitize_identifier_base(get_full_name(entity, true, false, "_"));
        }

        const class_entity* resolve_non_template_struct_entity(
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& type_name)
        {
            const auto normalised_type = normalise_cpp_type(type_name);
            std::shared_ptr<class_entity> resolved_entity;

            for (auto* scope = &current_scope; scope != nullptr; scope = scope->get_owner())
            {
                if (scope->find_class(normalised_type, resolved_entity))
                    break;
            }

            if (!resolved_entity && &current_scope != &lib)
                lib.find_class(normalised_type, resolved_entity);

            if (!resolved_entity || resolved_entity->get_entity_type() != entity_type::STRUCT
                || resolved_entity->get_is_template())
                return nullptr;

            return resolved_entity.get();
        }

        // Forward declaration for mutual recursion.
        bool is_supported_struct_for_protobuf_codegen(
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& type_name,
            std::set<std::string>& seen_structs);

        // Returns true if the type (already normalised) is codegen-supported:
        // scalars, strings, byte-vectors, vector<scalar>, vector<string>,
        // supported structs, or vector<supported-struct>.
        bool is_supported_field_type_for_protobuf_codegen(
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& field_type,
            std::set<std::string>& seen_structs)
        {
            if (is_supported_scalar_codegen_type(field_type))
                return true;

            std::string inner;
            if (extract_vector_value_type(field_type, inner))
                return is_supported_scalar_codegen_type(inner)
                       || is_supported_struct_for_protobuf_codegen(current_scope, lib, inner, seen_structs);

            return is_supported_struct_for_protobuf_codegen(current_scope, lib, field_type, seen_structs);
        }

        // Returns true if the named struct exists and every non-static field can be
        // encoded/decoded: scalars, strings, byte-vectors, nested supported structs, and
        // vectors thereof.
        bool is_supported_struct_for_protobuf_codegen(
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& type_name,
            std::set<std::string>& seen_structs)
        {
            const auto* struct_entity = resolve_non_template_struct_entity(current_scope, lib, type_name);
            if (!struct_entity)
                return false;

            const auto struct_key = qualified_cpp_type_name(*struct_entity);
            if (!seen_structs.insert(struct_key).second)
                return true;

            for (auto& member : struct_entity->get_elements(entity_type::STRUCTURE_MEMBERS))
            {
                if (!member || member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;
                auto func_entity = std::static_pointer_cast<function_entity>(member);
                if (func_entity->is_static())
                    continue;
                if (!is_supported_field_type_for_protobuf_codegen(
                        *struct_entity, lib, normalise_cpp_type(func_entity->get_return_type()), seen_structs))
                    return false;
            }
            return true;
        }

        bool is_supported_protobuf_codegen_type_with_lib(
            const std::string& cpp_type,
            const class_entity& current_scope,
            const class_entity& lib)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            bool is_optimistic = false;
            std::shared_ptr<class_entity> interface_entity;
            if (is_interface_param(current_scope, type_name, is_optimistic, interface_entity))
                return true;

            if (is_supported_scalar_codegen_type(type_name))
                return true;

            std::set<std::string> seen_structs;
            std::string inner_type;
            if (extract_vector_value_type(type_name, inner_type))
                return is_supported_scalar_codegen_type(inner_type)
                       || is_supported_struct_for_protobuf_codegen(current_scope, lib, inner_type, seen_structs);

            return is_supported_struct_for_protobuf_codegen(current_scope, lib, type_name, seen_structs);
        }

        bool supports_basic_protobuf_method_codegen_with_lib(
            const function_entity& function,
            const class_entity& current_scope,
            const class_entity& lib)
        {
            for (const auto& parameter : function.get_parameters())
            {
                if (!is_supported_protobuf_codegen_type_with_lib(parameter.get_type(), current_scope, lib))
                    return false;
            }

            const auto return_type = normalise_cpp_type(function.get_return_type());
            return return_type.empty() || return_type == "void"
                   || is_supported_protobuf_codegen_type_with_lib(return_type, current_scope, lib);
        }

        std::string protobuf_message_rust_type(
            const std::string& root_module_name,
            const std::string& message_name)
        {
            return "crate::__canopy_protobuf::" + sanitize_identifier(root_module_name) + "::" + message_name;
        }

        // struct_helper_prefix is the module path prefix for the from_proto_X / to_proto_X
        // helpers. Use "super::" from inside a method sub-module, or "" when called from
        // within interface_binding itself (e.g. from a nested struct helper body).
        std::string protobuf_getter_expression(
            const std::string& message_name,
            const std::string& cpp_type,
            const std::string& field_name,
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& struct_helper_prefix = "super::")
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            bool is_optimistic = false;
            std::shared_ptr<class_entity> interface_entity;
            if (is_interface_param(current_scope, type_name, is_optimistic, interface_entity))
                return "canopy_rpc::RemoteObject::from(canopy_rpc::ZoneAddress::new(" + message_name + "." + field_name
                       + "().addr_().blob().as_ref().to_vec()))";

            if (type_name == "std::vector<uint8_t>" || type_name == "std::vector<unsigned char>")
                return "canopy_rpc::serialization::protobuf::deserialize_bytes(" + message_name + "." + field_name + "())";
            if (type_name == "std::vector<char>" || type_name == "std::vector<signed char>")
                return "canopy_rpc::serialization::protobuf::deserialize_signed_bytes(" + message_name + "."
                       + field_name + "())";
            std::string vector_value_type;
            if (extract_vector_value_type(type_name, vector_value_type))
            {
                if (vector_value_type == "std::string")
                    return message_name + "." + field_name + "().into_iter().map(|value| value.to_string()).collect()";
                if (vector_value_type == "size_t")
                    return message_name + "."
                           + field_name + "().into_iter().map(|value| usize::try_from(value).map_err(|_| canopy_rpc::INVALID_DATA())).collect::<Result<Vec<_>, _>>()?";
                const auto* inner_struct = resolve_non_template_struct_entity(current_scope, lib, vector_value_type);
                if (inner_struct)
                    return message_name + "." + field_name + "().into_iter().map(|m| " + struct_helper_prefix
                           + "from_proto_" + protobuf_struct_helper_name(*inner_struct) + "(m.to_owned())).collect()";
                return message_name + "." + field_name + "().into_iter().collect()";
            }
            if (type_name == "std::string")
                return message_name + "." + field_name + "().to_string()";
            const auto* struct_entity = resolve_non_template_struct_entity(current_scope, lib, type_name);
            if (struct_entity)
                return struct_helper_prefix + "from_proto_" + protobuf_struct_helper_name(*struct_entity) + "("
                       + message_name + "." + field_name + "().to_owned())";
            return message_name + "." + field_name + "()";
        }

        std::string protobuf_setter_argument_expression(
            const std::string& value_name,
            const std::string& cpp_type,
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& struct_helper_prefix = "super::")
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            bool is_optimistic = false;
            std::shared_ptr<class_entity> interface_entity;
            if (is_interface_param(current_scope, type_name, is_optimistic, interface_entity))
            {
                return "{ let mut remote_object = crate::__canopy_protobuf::rpc::remote_object::new(); "
                       "let mut zone_address = crate::__canopy_protobuf::rpc::zone_address::new(); "
                       "zone_address.set_blob("
                       + value_name
                       + ".get_address().get_blob().to_vec()); "
                         "remote_object.set_addr_(zone_address); remote_object }";
            }

            if (type_name == "std::vector<uint8_t>" || type_name == "std::vector<unsigned char>")
                return "canopy_rpc::serialization::protobuf::serialize_bytes(&" + value_name + ")";
            if (type_name == "std::vector<char>" || type_name == "std::vector<signed char>")
                return "canopy_rpc::serialization::protobuf::serialize_signed_bytes(&" + value_name + ")";
            std::string vector_value_type;
            if (extract_vector_value_type(type_name, vector_value_type))
            {
                if (vector_value_type == "std::string")
                    return value_name + ".iter().map(|value| value.as_str())";
                if (vector_value_type == "size_t")
                    return value_name + ".iter().map(|value| u64::try_from(*value).map_err(|_| canopy_rpc::INVALID_DATA())).collect::<Result<Vec<_>, _>>()?";
                const auto* inner_struct = resolve_non_template_struct_entity(current_scope, lib, vector_value_type);
                if (inner_struct)
                    return value_name + ".iter().map(|v| " + struct_helper_prefix + "to_proto_"
                           + protobuf_struct_helper_name(*inner_struct) + "(v))";
                return value_name + ".iter().copied()";
            }
            if (type_name == "std::string")
                return value_name + ".as_str()";
            if (type_name == "size_t")
                return "u64::try_from(" + value_name + ").map_err(|_| canopy_rpc::INVALID_DATA())?";
            const auto* struct_entity = resolve_non_template_struct_entity(current_scope, lib, type_name);
            if (struct_entity)
                return struct_helper_prefix + "to_proto_" + protobuf_struct_helper_name(*struct_entity) + "(&"
                       + value_name + ")";
            return value_name;
        }

        // Centralized field conversion helpers.
        // These drive all protobuf conversion from the schema shape of the generated
        // message types, so adding new IDL field types only requires updating these
        // helpers rather than duplicating logic in multiple places.

        struct field_conversion_context
        {
            const class_entity& current_scope;
            const class_entity& lib;
            const std::string& root_module_name;
            const std::string& struct_helper_prefix;
        };

        // Generate a Rust expression that reads a field from a protobuf message
        // and converts it to the corresponding Rust type.
        std::string generate_field_from_proto(
            const field_conversion_context& ctx,
            const std::string& message_name,
            const std::string& cpp_type,
            const std::string& field_name)
        {
            return protobuf_getter_expression(
                message_name, cpp_type, field_name, ctx.current_scope, ctx.lib, ctx.struct_helper_prefix);
        }

        // Generate a Rust expression that converts a Rust value and sets it
        // on a protobuf message field.
        std::string generate_field_to_proto(
            const field_conversion_context& ctx,
            const std::string& value_name,
            const std::string& cpp_type)
        {
            return protobuf_setter_argument_expression(
                value_name, cpp_type, ctx.current_scope, ctx.lib, ctx.struct_helper_prefix);
        }

        // Generate from_proto_X / to_proto_X conversion for a struct type.
        // This is the canonical struct conversion path that method parameter
        // conversion delegates to when a parameter is a struct type.
        void emit_struct_conversion_helpers(
            writer& output,
            const field_conversion_context& ctx,
            const class_entity& struct_entity)
        {
            const auto struct_name = protobuf_struct_helper_name(struct_entity);
            const auto rust_struct_type
                = "crate::" + ctx.root_module_name + "::" + qualified_public_type_path(struct_entity);
            const auto proto_struct_type
                = protobuf_message_rust_type(ctx.root_module_name, sanitize_identifier(struct_entity.get_name()));

            // Generate from_proto: convert protobuf message -> Rust struct
            // Driven by the protobuf message's field shape via its generated getters
            output("fn from_proto_{}(msg: {}) -> {}", struct_name, proto_struct_type, rust_struct_type);
            output("{{");
            output("\t{} {{", rust_struct_type);
            for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
            {
                if (!member || member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;
                auto func_entity = std::static_pointer_cast<function_entity>(member);
                if (func_entity->is_static())
                    continue;
                const auto field_name = sanitize_identifier(func_entity->get_name());
                output(
                    "\t\t{}: {},",
                    field_name,
                    generate_field_from_proto(ctx, "msg", func_entity->get_return_type(), field_name));
            }
            output("\t}}");
            output("}}");
            output("");

            // Generate to_proto: convert Rust struct -> protobuf message
            // Driven by the protobuf message's field shape via its generated setters
            output("fn to_proto_{}(val: &{}) -> {}", struct_name, rust_struct_type, proto_struct_type);
            output("{{");
            output("\tlet mut msg = {}::new();", proto_struct_type);
            for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
            {
                if (!member || member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;
                auto func_entity = std::static_pointer_cast<function_entity>(member);
                if (func_entity->is_static())
                    continue;
                const auto field_name = sanitize_identifier(func_entity->get_name());
                output(
                    "\tmsg.set_{}({});",
                    field_name,
                    generate_field_to_proto(ctx, "val." + field_name, func_entity->get_return_type()));
            }
            output("\tmsg");
            output("}}");
            output("");
        }

        // Collect unique struct entities used (directly or transitively via fields) by any
        // supported-codegen method in the interface.  Order is depth-first so that a struct
        // is always emitted before the structs that reference it.
        std::vector<const class_entity*> collect_struct_types_for_interface(
            const class_entity& lib,
            const class_entity& iface)
        {
            std::vector<const class_entity*> result;
            std::set<std::string> seen;

            std::function<void(const class_entity*)> add_struct = [&](const class_entity* struct_entity)
            {
                if (!struct_entity)
                    return;
                if (!seen.insert(qualified_cpp_type_name(*struct_entity)).second)
                    return;
                // Recurse into field types first so dependencies are emitted before users.
                for (auto& member : struct_entity->get_elements(entity_type::STRUCTURE_MEMBERS))
                {
                    if (!member || member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                        continue;
                    auto func_entity = std::static_pointer_cast<function_entity>(member);
                    if (func_entity->is_static())
                        continue;
                    const auto field_type = normalise_cpp_type(func_entity->get_return_type());
                    add_struct(resolve_non_template_struct_entity(*struct_entity, lib, field_type));
                    std::string inner;
                    if (extract_vector_value_type(field_type, inner))
                        add_struct(resolve_non_template_struct_entity(*struct_entity, lib, inner));
                }
                result.push_back(struct_entity);
            };

            auto maybe_add_type = [&](const std::string& cpp_type)
            {
                const auto type_name = normalise_cpp_type(cpp_type);
                add_struct(resolve_non_template_struct_entity(iface, lib, type_name));
                std::string inner;
                if (extract_vector_value_type(type_name, inner))
                    add_struct(resolve_non_template_struct_entity(iface, lib, inner));
            };

            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                if (!supports_basic_protobuf_method_codegen_with_lib(*function, iface, lib))
                    continue;
                for (const auto& parameter : function->get_parameters())
                    maybe_add_type(parameter.get_type());
                const auto return_type = normalise_cpp_type(function->get_return_type());
                if (!return_type.empty() && return_type != "void")
                    maybe_add_type(return_type);
            }
            return result;
        }

        struct build_target_descriptor
        {
            std::string crate_name;
            std::string output_subdir;
            std::string manifest_path;
            std::string master_proto;
        };

        build_target_descriptor self_build_target_descriptor(
            const std::filesystem::path& sub_directory,
            const std::string& base_filename)
        {
            auto module_name = sanitize_identifier(base_filename);
            const auto parent_name = sub_directory.parent_path().filename().string();
            if (!parent_name.empty())
                module_name = sanitize_identifier(parent_name);
            return {
                "crate::" + module_name,
                module_name,
                (sub_directory / "manifest.txt").generic_string(),
                (sub_directory / (base_filename + "_all.proto")).generic_string(),
            };
        }

        std::vector<build_target_descriptor> imported_build_target_descriptors(
            const class_entity& lib,
            const build_target_descriptor& self_target)
        {
            std::set<std::string> seen_manifests;
            std::vector<build_target_descriptor> result;

            auto add_target = [&](const build_target_descriptor& target)
            {
                if (target.manifest_path == self_target.manifest_path)
                    return;
                if (!seen_manifests.insert(target.manifest_path).second)
                    return;
                result.push_back(target);
            };

            add_target(
                {
                    "crate::rpc",
                    "rpc",
                    "rpc/protobuf/manifest.txt",
                    "rpc/protobuf/rpc_types_all.proto",
                });

            for (auto& cls : lib.get_classes())
            {
                if (!cls || cls->get_import_lib().empty())
                    continue;

                auto import_lib = cls->get_import_lib();
                const auto idl_suffix = import_lib.rfind(".idl");
                if (idl_suffix == std::string::npos)
                    continue;

                std::filesystem::path import_path(import_lib);
                const auto base_name = import_path.stem().string();
                const auto sub_directory = import_path.parent_path() / "protobuf";
                const auto manifest_path = (sub_directory / "manifest.txt").generic_string();
                add_target(
                    {
                        "crate::" + sanitize_identifier(base_name),
                        sanitize_identifier(base_name),
                        manifest_path,
                        (sub_directory / (base_name + "_all.proto")).generic_string(),
                    });
            }

            return result;
        }

        void write_build_metadata_file(
            const std::filesystem::path& rust_fs_path,
            const build_target_descriptor& self_target,
            const std::vector<build_target_descriptor>& imported_targets)
        {
            auto build_metadata_path = rust_fs_path;
            build_metadata_path.replace_filename(rust_fs_path.stem().string() + "_build.rs");

            std::string existing_data;
            {
                std::ifstream existing_file(build_metadata_path);
                std::getline(existing_file, existing_data, '\0');
            }

            std::stringstream stream;
            writer output(stream);
            output("// Generated by Canopy IDL generator. Do not edit.");
            output("// Lightweight Rust protobuf build metadata for build.rs consumers.");
            output("");
            output("#[derive(Debug, Clone, Copy, PartialEq, Eq)]");
            output("pub struct ProtobufBuildTarget");
            output("{{");
            output("\tpub crate_name: &'static str,");
            output("\tpub output_subdir: &'static str,");
            output("\tpub manifest_path: &'static str,");
            output("\tpub master_proto: &'static str,");
            output("}}");
            output("");
            output("pub const BUILD_TARGET: ProtobufBuildTarget = ProtobufBuildTarget");
            output("{{");
            output("\tcrate_name: \"{}\",", self_target.crate_name);
            output("\toutput_subdir: \"{}\",", self_target.output_subdir);
            output("\tmanifest_path: \"{}\",", self_target.manifest_path);
            output("\tmaster_proto: \"{}\",", self_target.master_proto);
            output("}};");
            output("");
            output("pub const BUILD_DEPENDENCIES: &[ProtobufBuildTarget] = &[");
            for (const auto& target : imported_targets)
            {
                output(
                    "\tProtobufBuildTarget {{ crate_name: \"{}\", output_subdir: \"{}\", manifest_path: \"{}\", "
                    "master_proto: \"{}\" }},",
                    target.crate_name,
                    target.output_subdir,
                    target.manifest_path,
                    target.master_proto);
            }
            output("];");
            output("");
            output("pub fn all_build_targets() -> Vec<ProtobufBuildTarget>");
            output("{{");
            output("\tlet mut targets = Vec::with_capacity(BUILD_DEPENDENCIES.len() + 1);");
            output("\ttargets.push(BUILD_TARGET);");
            output("\ttargets.extend_from_slice(BUILD_DEPENDENCIES);");
            output("\ttargets");
            output("}}");

            if (stream.str() != existing_data)
            {
                std::ofstream file(build_metadata_path);
                file << stream.str();
            }
        }

        void write_interface(
            writer& output,
            const class_entity& lib,
            const class_entity& iface,
            const std::string& root_module_name)
        {
            const auto proto_package = protobuf_package_for_interface(iface);
            const auto schema_proto_file = protobuf_schema_file_for_interface(iface);

            output("#[allow(non_snake_case)]");
            output("pub mod {}", upper_camel_identifier(iface.get_name()));
            output("{{");
            output("pub mod interface_binding");
            output("{{");
            output("pub struct BindingMetadata;");
            output("");

            // Emit from_proto_X / to_proto_X helpers for every struct type used by
            // any supported-codegen method in this interface.
            // These helpers are driven by the centralized struct conversion path.
            const auto interface_structs = collect_struct_types_for_interface(lib, iface);
            const field_conversion_context struct_ctx{iface, lib, root_module_name, ""};
            for (const auto* struct_entity : interface_structs)
            {
                emit_struct_conversion_helpers(output, struct_ctx, *struct_entity);
            }

            int method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto module_name = sanitize_identifier(function->get_name());
                const auto request_message = iface.get_name() + "_" + function->get_name() + "Request";
                const auto response_message = iface.get_name() + "_" + function->get_name() + "Response";
                const auto interface_generics = analyse_interface_generics(iface, *function, root_module_name);
                const auto request_generics = generic_declaration_for_params(interface_generics, true, false);
                const auto response_generics = generic_declaration_for_params(interface_generics, false, true);
                const auto codec_generics = generic_declaration_for_params(interface_generics, true, true);
                const auto codec_where = generic_where_clause_for_params(interface_generics, true, true);
                const auto interface_module_path = "crate::" + root_module_name
                                                   + "::" + qualified_generated_interface_path(iface)
                                                   + "::interface_binding::" + module_name;
                const auto interface_trait_path = "crate::" + root_module_name + "::" + qualified_public_type_path(iface);
                const auto proto_request_type = protobuf_message_rust_type(root_module_name, request_message);
                const auto proto_response_type = protobuf_message_rust_type(root_module_name, response_message);
                const bool supports_dispatch_codegen
                    = supports_basic_protobuf_method_codegen_with_lib(*function, iface, lib);
                // Interface params use caller-aware encode/decode hooks for local outgoing
                // bindings and remote proxy construction.
                const bool supports_proxy_codegen = supports_dispatch_codegen;
                bool has_interface_request_params = false;
                bool has_interface_response_params = false;
                for (const auto& generic : interface_generics)
                {
                    has_interface_request_params = has_interface_request_params || generic.is_in;
                    has_interface_response_params = has_interface_response_params || generic.is_out;
                }

                // When there are [out]-only interface params, Request carries combined generics
                // so we need to use the combined generics here too.
                bool has_out_only_interface = false;
                for (const auto& generic : interface_generics)
                {
                    if (generic.is_out && !generic.is_in)
                    {
                        has_out_only_interface = true;
                        break;
                    }
                }
                const auto effective_request_generics
                    = has_out_only_interface ? generic_declaration_for_params(interface_generics, true, true)
                                             : request_generics;
                bool has_request_fields = false;
                bool has_response_fields
                    = !function->get_return_type().empty() && normalise_cpp_type(function->get_return_type()) != "void";
                for (const auto& parameter : function->get_parameters())
                {
                    has_request_fields = has_request_fields || is_in_param(parameter)
                                         || (!is_in_param(parameter) && !is_out_param(parameter));
                    has_response_fields = has_response_fields || is_out_param(parameter);
                }

                output("pub mod {}", module_name);
                output("{{");
                output("pub const PROTOBUF_PACKAGE: &str = \"{}\";", proto_package);
                output("pub const PROTOBUF_SCHEMA_FILE: &str = \"{}\";", schema_proto_file);
                output("pub const PROTOBUF_REQUEST_MESSAGE: &str = \"{}\";", request_message);
                output("pub const PROTOBUF_RESPONSE_MESSAGE: &str = \"{}\";", response_message);
                output("pub const PROTOBUF_REQUEST_FULL_NAME: &str = \"{}.{}\";", proto_package, request_message);
                output("pub const PROTOBUF_RESPONSE_FULL_NAME: &str = \"{}.{}\";", proto_package, response_message);
                output("pub const PROTOBUF_REQUEST_RUST_TYPE: &str = \"{}\";", request_message);
                output("pub const PROTOBUF_RESPONSE_RUST_TYPE: &str = \"{}\";", response_message);
                output("pub const PROTOBUF_PARAMS: &[canopy_rpc::serialization::protobuf::GeneratedProtobufParamDescriptor] = &[");

                uint32_t protobuf_field_number = 1;
                for (const auto& parameter : function->get_parameters())
                {
                    const bool param_is_in
                        = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                    const auto param_type = normalise_cpp_type(parameter.get_type());
                    bool is_optimistic = false;
                    std::shared_ptr<class_entity> interface_entity;
                    const bool is_interface = is_interface_param(iface, param_type, is_optimistic, interface_entity);
                    const auto pointer_kind
                        = is_interface
                              ? (is_optimistic
                                        ? "Some(canopy_rpc::serialization::protobuf::InterfacePointerKind::Optimistic)"
                                        : "Some(canopy_rpc::serialization::protobuf::InterfacePointerKind::Shared)")
                              : "None";
                    output(
                        "\tcanopy_rpc::serialization::protobuf::GeneratedProtobufParamDescriptor {{ name: \"{}\", "
                        "field_number: {}u32, direction: "
                        "canopy_rpc::ParameterDirection::{}, proto_type: \"{}\", field_kind: "
                        "canopy_rpc::serialization::protobuf::GeneratedProtobufFieldKind::{}, pointer_kind: {} }},",
                        parameter.get_name(),
                        protobuf_field_number,
                        direction_name(param_is_in, is_out_param(parameter)),
                        cpp_type_to_proto_type(iface, lib, parameter.get_type()),
                        protobuf_field_kind(iface, lib, parameter.get_type()),
                        pointer_kind);
                    ++protobuf_field_number;
                }
                output("];");
                output("");
                output(
                    "pub struct ProtobufCodec{}(std::marker::PhantomData<fn() -> {}>);",
                    codec_generics,
                    phantom_return_type_for_generics(codec_generics));
                output("");
                output(
                    "impl{} canopy_rpc::serialization::protobuf::GeneratedProtobufMethodCodec for ProtobufCodec{}{}",
                    codec_generics,
                    codec_generics,
                    codec_where);
                output("{{");
                output("\ttype ProxyRequest = {}::Request{};", interface_module_path, effective_request_generics);
                output("\ttype ProxyResponse = {}::Response{};", interface_module_path, response_generics);
                output("\ttype DispatchRequest = {}::DispatchRequest;", interface_module_path);
                output("\ttype DispatchResponse = {}::DispatchResponse;", interface_module_path);
                if (supports_dispatch_codegen)
                {
                    output("\ttype ProtoRequest = {};", proto_request_type);
                    output("\ttype ProtoResponse = {};", proto_response_type);
                }
                else
                {
                    output("\ttype ProtoRequest = canopy_rpc::serialization::protobuf::UnsupportedGeneratedMessage;");
                    output("\ttype ProtoResponse = canopy_rpc::serialization::protobuf::UnsupportedGeneratedMessage;");
                }
                output("");
                output("\tfn descriptor() -> &'static canopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor");
                output("\t{{");
                output("\t\tsuper::protobuf_by_method_id({}u64).expect(\"protobuf method descriptor\")", method_count);
                output("\t}}");
                output("");
                if (supports_dispatch_codegen)
                {
                    output(
                        "\tfn request_from_protobuf_message({}: Self::ProtoRequest) -> Result<Self::DispatchRequest, "
                        "i32>",
                        has_request_fields ? "message" : "_message");
                    output("\t{{");
                    output("\t\tOk(Self::DispatchRequest {{");
                    for (const auto& parameter : function->get_parameters())
                    {
                        const bool param_is_in
                            = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                        if (!param_is_in)
                            continue;
                        output(
                            "\t\t\t{}: {},",
                            sanitize_identifier(parameter.get_name()),
                            protobuf_getter_expression(
                                "message", parameter.get_type(), sanitize_identifier(parameter.get_name()), iface, lib));
                    }
                    output("\t\t}})");
                    output("\t}}");
                    output("");
                }
                else
                {
                    output("\tfn request_from_protobuf_message(_message: Self::ProtoRequest) -> Result<Self::DispatchRequest, i32>");
                    output("\t{{");
                    output("\t\tErr(canopy_rpc::INVALID_DATA())");
                    output("\t}}");
                    output("");
                }

                if (supports_proxy_codegen)
                {
                    output(
                        "\tfn request_to_protobuf_message({}: &Self::ProxyRequest) -> Result<Self::ProtoRequest, i32>",
                        (has_request_fields && interface_generics.empty()) ? "request" : "_request");
                    output("\t{{");
                    if (!interface_generics.empty())
                    {
                        output("\t\tErr(canopy_rpc::TRANSPORT_ERROR())");
                    }
                    else
                    {
                        output("\t\tlet{} message = Self::ProtoRequest::new();", has_request_fields ? " mut" : "");
                        for (const auto& parameter : function->get_parameters())
                        {
                            const bool param_is_in
                                = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                            if (!param_is_in)
                                continue;
                            const auto field_name = sanitize_identifier(parameter.get_name());
                            output(
                                "\t\tmessage.set_{}({});",
                                field_name,
                                protobuf_setter_argument_expression(
                                    "request." + field_name, parameter.get_type(), iface, lib));
                        }
                        output("\t\tOk(message)");
                    }
                    output("\t}}");
                    output("");
                    if (!interface_generics.empty())
                    {
                        output(
                            "\tfn request_to_protobuf_message_with_caller({}: &dyn canopy_rpc::GeneratedRpcCaller, "
                            "{}: &Self::ProxyRequest) -> Result<Self::ProtoRequest, i32>",
                            has_interface_request_params ? "caller" : "_caller",
                            has_request_fields ? "request" : "_request");
                        output("\t{{");
                        output("\t\tlet{} message = Self::ProtoRequest::new();", has_request_fields ? " mut" : "");
                        if (has_interface_request_params)
                        {
                            output("\t\tlet Some(service) = caller.local_service() else");
                            output("\t\t{{");
                            output("\t\t\treturn Err(canopy_rpc::TRANSPORT_ERROR());");
                            output("\t\t}};");
                            output("\t\tlet context = caller.call_context();");
                        }
                        for (const auto& parameter : function->get_parameters())
                        {
                            const bool param_is_in
                                = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                            if (!param_is_in)
                                continue;

                            const auto field_name = sanitize_identifier(parameter.get_name());
                            bool is_optimistic = false;
                            std::shared_ptr<class_entity> interface_entity;
                            if (is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                            {
                                output(
                                    "\t\tlet {}_binding = if request.{}.as_inner().as_ref().is_some_and(|iface| "
                                    "!canopy_rpc::BindableInterfaceValue::is_local(iface)) {{",
                                    field_name,
                                    field_name,
                                    field_name);
                                output(
                                    "\t\t\t{}::bind_{}_outgoing_remote(context.caller_zone_id.clone(), &request.{})",
                                    interface_module_path,
                                    field_name,
                                    field_name);
                                output("\t\t}} else {{");
                                output(
                                    "\t\t\t{}::bind_{}_outgoing_local(service, context.caller_zone_id.clone(), "
                                    "&request.{})",
                                    interface_module_path,
                                    field_name,
                                    field_name);
                                output("\t\t}};");
                                output("\t\tif canopy_rpc::is_critical({}_binding.error_code)", field_name);
                                output("\t\t{{");
                                output("\t\t\treturn Err({}_binding.error_code);", field_name);
                                output("\t\t}}");
                                output(
                                    "\t\tmessage.set_{}({});",
                                    field_name,
                                    protobuf_setter_argument_expression(
                                        field_name + "_binding.descriptor", parameter.get_type(), iface, lib));
                            }
                            else
                            {
                                output(
                                    "\t\tmessage.set_{}({});",
                                    field_name,
                                    protobuf_setter_argument_expression(
                                        "request." + field_name, parameter.get_type(), iface, lib));
                            }
                        }
                        output("\t\tOk(message)");
                        output("\t}}");
                        output("");
                    }
                }
                else
                {
                    output("\tfn request_to_protobuf_message(_request: &Self::ProxyRequest) -> Result<Self::ProtoRequest, i32>");
                    output("\t{{");
                    output("\t\tErr(canopy_rpc::INVALID_DATA())");
                    output("\t}}");
                    output("");
                }

                if (supports_dispatch_codegen)
                {
                    output(
                        "\tfn response_to_protobuf_message({}: &Self::DispatchResponse) -> Result<Self::ProtoResponse, "
                        "i32>",
                        has_response_fields ? "response" : "_response");
                    output("\t{{");
                    output("\t\tlet{} message = Self::ProtoResponse::new();", has_response_fields ? " mut" : "");
                    for (const auto& parameter : function->get_parameters())
                    {
                        if (!is_out_param(parameter))
                            continue;
                        const auto field_name = sanitize_identifier(parameter.get_name());
                        output(
                            "\t\tmessage.set_{}({});",
                            field_name,
                            protobuf_setter_argument_expression("response." + field_name, parameter.get_type(), iface, lib));
                    }
                    if (!function->get_return_type().empty() && normalise_cpp_type(function->get_return_type()) != "void")
                        output(
                            "\t\tmessage.set_result({});",
                            protobuf_setter_argument_expression(
                                "response.return_value", function->get_return_type(), iface, lib));
                    output("\t\tOk(message)");
                    output("\t}}");
                    output("");
                }
                else
                {
                    output("\tfn response_to_protobuf_message(_response: &Self::DispatchResponse) -> Result<Self::ProtoResponse, i32>");
                    output("\t{{");
                    output("\t\tErr(canopy_rpc::INVALID_DATA())");
                    output("\t}}");
                    output("");
                }

                if (supports_proxy_codegen)
                {
                    output(
                        "\tfn response_from_protobuf_message({}: Self::ProtoResponse) -> Result<Self::ProxyResponse, "
                        "i32>",
                        has_interface_response_params ? "_message" : "message");
                    output("\t{{");
                    if (has_interface_response_params)
                    {
                        output("\t\tErr(canopy_rpc::TRANSPORT_ERROR())");
                    }
                    else
                    {
                        output("\t\tOk(Self::ProxyResponse {{");
                        for (const auto& parameter : function->get_parameters())
                        {
                            if (!is_out_param(parameter))
                                continue;
                            output(
                                "\t\t\t{}: {},",
                                sanitize_identifier(parameter.get_name()),
                                protobuf_getter_expression(
                                    "message", parameter.get_type(), sanitize_identifier(parameter.get_name()), iface, lib));
                        }
                        if (!function->get_return_type().empty() && normalise_cpp_type(function->get_return_type()) != "void")
                            output(
                                "\t\t\treturn_value: {},",
                                protobuf_getter_expression("message", function->get_return_type(), "result", iface, lib));
                        output("\t\t}})");
                    }
                    output("\t}}");
                    if (has_interface_response_params)
                    {
                        output("");
                        output(
                            "\tfn response_from_protobuf_message_with_caller(caller: &dyn "
                            "canopy_rpc::GeneratedRpcCaller, "
                            "message: Self::ProtoResponse) -> Result<Self::ProxyResponse, i32>");
                        output("\t{{");
                        for (const auto& parameter : function->get_parameters())
                        {
                            if (!is_out_param(parameter))
                                continue;

                            const auto field_name = sanitize_identifier(parameter.get_name());
                            bool is_optimistic = false;
                            std::shared_ptr<class_entity> interface_entity;
                            if (is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                            {
                                std::string generic_name;
                                for (const auto& generic : interface_generics)
                                {
                                    if (generic.param_name == field_name && generic.is_out)
                                    {
                                        generic_name = generic.generic_name;
                                        break;
                                    }
                                }

                                output(
                                    "\t\tlet {}_remote_object = {};",
                                    field_name,
                                    protobuf_getter_expression("message", parameter.get_type(), field_name, iface, lib));
                                output(
                                    "\t\tlet {} = if {}_remote_object == canopy_rpc::null_remote_descriptor() || "
                                    "!{}_remote_object.is_set()",
                                    field_name,
                                    field_name,
                                    field_name);
                                output("\t\t{{");
                                output(
                                    "\t\t\t{}",
                                    is_optimistic ? "canopy_rpc::Optimistic::null()" : "canopy_rpc::Shared::null()");
                                output("\t\t}}");
                                output(
                                    "\t\telse if let Some(service) = caller.local_service().filter(|service| "
                                    "{}_remote_object.as_zone() == service.zone_id())",
                                    field_name);
                                output("\t\t{{");
                                output(
                                    "\t\t\tlet {}_binding = {}::bind_{}_incoming_local::<{}>(service, "
                                    "&{}_remote_object);",
                                    field_name,
                                    interface_module_path,
                                    field_name,
                                    generic_name,
                                    field_name);
                                output("\t\t\tif canopy_rpc::is_critical({}_binding.error_code)", field_name);
                                output("\t\t\t{{");
                                output("\t\t\t\treturn Err({}_binding.error_code);", field_name);
                                output("\t\t\t}}");
                                output(
                                    "\t\t\t{}::from_inner({}_binding.iface)",
                                    is_optimistic ? "canopy_rpc::Optimistic" : "canopy_rpc::Shared",
                                    field_name);
                                output("\t\t}}");
                                output("\t\telse");
                                output("\t\t{{");
                                output(
                                    "\t\t\tlet {}_caller = "
                                    "caller.make_remote_caller_with_ref({}_remote_object.clone(), "
                                    "canopy_rpc::InterfacePointerKind::{}).ok_or(canopy_rpc::TRANSPORT_ERROR())?;",
                                    field_name,
                                    field_name,
                                    is_optimistic ? "Optimistic" : "Shared");
                                output(
                                    "\t\t\tlet {}_proxy = std::sync::Arc::new(<{} as "
                                    "canopy_rpc::GeneratedRustInterface>::create_remote_proxy({}_caller));",
                                    field_name,
                                    generic_name,
                                    field_name);
                                output(
                                    "\t\t\t{}",
                                    is_optimistic ? "canopy_rpc::Optimistic::from_value(canopy_rpc::LocalProxy::"
                                                    "from_remote("
                                                        + field_name + "_proxy))"
                                                  : "canopy_rpc::Shared::from_value(" + field_name + "_proxy)");
                                output("\t\t}};");
                            }
                        }
                        output("\t\tOk(Self::ProxyResponse {{");
                        for (const auto& parameter : function->get_parameters())
                        {
                            if (!is_out_param(parameter))
                                continue;
                            const auto field_name = sanitize_identifier(parameter.get_name());
                            bool is_optimistic = false;
                            std::shared_ptr<class_entity> interface_entity;
                            if (is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                            {
                                output("\t\t\t{}: {},", field_name, field_name);
                            }
                            else
                            {
                                output(
                                    "\t\t\t{}: {},",
                                    field_name,
                                    protobuf_getter_expression("message", parameter.get_type(), field_name, iface, lib));
                            }
                        }
                        if (!function->get_return_type().empty() && normalise_cpp_type(function->get_return_type()) != "void")
                            output(
                                "\t\t\treturn_value: {},",
                                protobuf_getter_expression("message", function->get_return_type(), "result", iface, lib));
                        output("\t\t}})");
                        output("\t}}");
                    }
                }
                else
                {
                    output("\tfn response_from_protobuf_message(_message: Self::ProtoResponse) -> Result<Self::ProxyResponse, i32>");
                    output("\t{{");
                    output("\t\tErr(canopy_rpc::INVALID_DATA())");
                    output("\t}}");
                }
                output("}}");
                output("");
                output(
                    "pub fn encode_request{}(request: &{}::Request{}) -> Result<Vec<u8>, i32>{}",
                    codec_generics,
                    interface_module_path,
                    effective_request_generics,
                    codec_where);
                output("{{");
                output("\t<ProtobufCodec{} as canopy_rpc::serialization::protobuf::GeneratedProtobufMethodCodec>::encode_request(request)", codec_generics);
                output("}}");
                output("");
                output(
                    "pub fn decode_response{}(proto_bytes: &[u8]) -> Result<{}::Response{}, i32>{}",
                    codec_generics,
                    interface_module_path,
                    response_generics,
                    codec_where);
                output("{{");
                output("\t<ProtobufCodec{} as canopy_rpc::serialization::protobuf::GeneratedProtobufMethodCodec>::decode_response(proto_bytes)", codec_generics);
                output("}}");
                output("");
                output("#[doc(hidden)]");
                output(
                    "pub fn dispatch_generated<Impl{}>(implementation: &Impl, context: &canopy_rpc::DispatchContext, "
                    "params: canopy_rpc::SendParams) -> canopy_rpc::SendResult",
                    codec_generics.empty() ? "" : ", " + codec_generics.substr(1, codec_generics.size() - 2));
                if (codec_where.empty())
                    output(" where Impl: {}", interface_trait_path);
                else
                    output("{}, Impl: {}", codec_where, interface_trait_path);
                output("{{");
                output("\tcanopy_rpc::serialization::protobuf::dispatch_generated_stub_call::<ProtobufCodec{}, _>(params, |request|", codec_generics);
                output("\t{{");
                output(
                    "\t\t{}::dispatch_stub_request::<Impl{}>(implementation, context, request)",
                    interface_module_path,
                    codec_generics.empty() ? "" : ", " + codec_generics.substr(1, codec_generics.size() - 2));
                output("\t}})");
                output("}}");
                output("}}");
                output("");
                method_count++;
            }

            output("pub const PROTOBUF_METHODS: &[canopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor] = &[");
            method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto module_name = sanitize_identifier(function->get_name());
                output(
                    "\tcanopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor {{ method_name: \"{}\", "
                    "method_id: {}u64, "
                    "schema_proto_file: {}::PROTOBUF_SCHEMA_FILE, proto_package: {}::PROTOBUF_PACKAGE, "
                    "request_message: {}::PROTOBUF_REQUEST_MESSAGE, response_message: {}::PROTOBUF_RESPONSE_MESSAGE, "
                    "request_proto_full_name: {}::PROTOBUF_REQUEST_FULL_NAME, response_proto_full_name: "
                    "{}::PROTOBUF_RESPONSE_FULL_NAME, "
                    "request_rust_type_name: {}::PROTOBUF_REQUEST_RUST_TYPE, response_rust_type_name: "
                    "{}::PROTOBUF_RESPONSE_RUST_TYPE, "
                    "params: {}::PROTOBUF_PARAMS }},",
                    function->get_name(),
                    method_count,
                    module_name,
                    module_name,
                    module_name,
                    module_name,
                    module_name,
                    module_name,
                    module_name,
                    module_name,
                    module_name);
                method_count++;
            }
            output("];");
            output("");
            output("impl canopy_rpc::serialization::protobuf::GeneratedProtobufBindingMetadata for BindingMetadata");
            output("{{");
            output("\tfn protobuf_methods() -> &'static [canopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor]");
            output("\t{{");
            output("\t\tPROTOBUF_METHODS");
            output("\t}}");
            output("}}");
            output("");
            output("pub fn protobuf_by_method_id(method_id: u64) -> Option<&'static canopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor>");
            output("{{");
            output("\t<BindingMetadata as canopy_rpc::serialization::protobuf::GeneratedProtobufBindingMetadata>::protobuf_by_method_id(method_id)");
            output("}}");
            output("}}");
            output("}}");
            output("");
        }

        void write_namespace(
            writer& output,
            const class_entity& lib,
            const class_entity& scope,
            const std::string& root_module_name)
        {
            bool has_interfaces = false;
            for (auto& elem : scope.get_elements(entity_type::NAMESPACE_MEMBERS))
            {
                if (elem->is_in_import())
                    continue;

                if (elem->get_entity_type() == entity_type::NAMESPACE)
                {
                    auto& ns = static_cast<const class_entity&>(*elem);
                    output("pub mod {}", sanitize_identifier(ns.get_name()));
                    output("{{");
                    write_namespace(output, lib, ns, root_module_name);
                    output("}}");
                    output("");
                }
                else if (elem->get_entity_type() == entity_type::INTERFACE)
                {
                    has_interfaces = true;
                }
            }
            if (has_interfaces)
            {
                output("#[doc(hidden)]");
                output("#[allow(non_snake_case)]");
                output("pub mod __Generated");
                output("{{");
                for (auto& elem : scope.get_elements(entity_type::NAMESPACE_MEMBERS))
                {
                    if (elem->is_in_import())
                        continue;
                    if (elem->get_entity_type() != entity_type::INTERFACE)
                        continue;
                    write_interface(output, lib, static_cast<const class_entity&>(*elem), root_module_name);
                }
                output("}}");
            }
        }

        bool is_different(
            const std::stringstream& stream,
            const std::string& data)
        {
            return stream.str() != data;
        }
    } // namespace

    std::filesystem::path write_file(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& relative_path,
        const std::filesystem::path& sub_directory,
        const std::string& base_filename,
        const std::vector<std::string>& generated_proto_files)
    {
        auto rust_fs_path
            = output_path / "rust" / relative_path.parent_path() / (relative_path.stem().string() + "_protobuf.rs");
        std::filesystem::create_directories(rust_fs_path.parent_path());

        std::string existing_data;
        {
            std::ifstream existing_file(rust_fs_path);
            std::getline(existing_file, existing_data, '\0');
        }

        const auto master_proto = (sub_directory / (base_filename + "_all.proto")).generic_string();
        const auto manifest_path = (sub_directory / "manifest.txt").generic_string();
        const auto entry_module = sanitize_identifier(base_filename + "_all");
        const auto root_module_name = sanitize_identifier(base_filename);
        const auto generated_rs_files = generated_rs_candidates(generated_proto_files, master_proto);
        const auto self_target = self_build_target_descriptor(sub_directory, base_filename);
        const auto imported_targets = imported_build_target_descriptors(lib, self_target);

        std::stringstream stream;
        writer output(stream);
        output("// Generated by Canopy IDL generator. Do not edit.");
        output("// Rust protobuf metadata over the canonical Canopy .proto output.");
        output("//");
        output("// Intended build-time use:");
        output("// 1. read PROTO_INCLUDE_DIRS and PROTO_FILES");
        output("// 2. invoke the protobuf Rust generator from submodules/protobuf on those files");
        output("// 3. include the generated entry point/module from your build output");
        output("//");
        output("// Protobuf-specific Rust metadata lives in this file on purpose.");
        output("// Keep protobuf interpretation and message naming here rather than");
        output("// in generator/src/rust_generator.cpp.");
        output("");
        output("pub const MASTER_PROTO: &str = \"{}\";", master_proto);
        output("pub const PROTO_MANIFEST: &str = \"{}\";", manifest_path);
        output("pub const PROTO_INCLUDE_DIRS: &[&str] = &[\"src\"];");
        output("pub const PROTO_FILES: &[&str] = &[");
        for (const auto& proto_file : generated_proto_files)
            output("\t\"{}\",", proto_file);
        output("\t\"{}\",", master_proto);
        output("];");
        output("pub const GENERATED_RS_ENTRY_MODULE: &str = \"{}\";", entry_module);
        output("pub const GENERATED_RS_CPP_KERNEL_SUFFIX: &str = \".c.pb.rs\";");
        output("pub const GENERATED_RS_UPB_KERNEL_SUFFIX: &str = \".u.pb.rs\";");
        output("pub const GENERATED_RS_FILES: &[&str] = &[");
        for (const auto& generated_rs_file : generated_rs_files)
            output("\t\"{}\",", generated_rs_file);
        output("];");
        output("");
        output("pub fn all_proto_files() -> Vec<&'static str>");
        output("{{");
        output("\tPROTO_FILES.to_vec()");
        output("}}");
        output("");
        output("pub fn generated_rs_candidates() -> Vec<&'static str>");
        output("{{");
        output("\tGENERATED_RS_FILES.to_vec()");
        output("}}");
        output("");

        write_namespace(output, lib, lib, root_module_name);

        if (is_different(stream, existing_data))
        {
            std::ofstream file(rust_fs_path);
            file << stream.str();
        }

        write_build_metadata_file(rust_fs_path, self_target, imported_targets);

        return rust_fs_path;
    }
}
