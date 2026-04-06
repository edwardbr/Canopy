/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "rust_generator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "coreclasses.h"
#include "fingerprint_generator.h"
#include "helpers.h"
#include "proto_generator.h"
#include "writer.h"

namespace rust_generator
{
    namespace
    {
        struct protocol_version_descriptor
        {
            const char* name;
            uint64_t value;
        };

        constexpr protocol_version_descriptor protocol_versions[] = {
            {"V3", 3},
            {"V2", 2},
        };

        std::string sanitize_identifier(const std::string& name)
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

        std::string uppercase_identifier(const std::string& name)
        {
            auto result = sanitize_identifier(name);
            std::transform(
                result.begin(),
                result.end(),
                result.begin(),
                [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            return result;
        }

        std::string upper_camel_identifier(const std::string& name)
        {
            auto sanitized = sanitize_identifier(name);
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

        std::string qualified_rust_module_path(const class_entity& entity)
        {
            std::vector<std::string> parts;
            parts.push_back(sanitize_identifier(entity.get_name()));

            auto current = entity.get_owner();
            while (current != nullptr && !current->get_name().empty() && current->get_name() != "__global__")
            {
                parts.push_back(sanitize_identifier(current->get_name()));
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

        bool extract_vector_value_type(
            const std::string& cpp_type,
            std::string& inner_type)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            const auto prefix = std::string("std::vector<");
            if (type_name.rfind(prefix, 0) != 0)
                return false;

            std::string template_content;
            const auto end_pos = proto_generator::extract_template_content(type_name, prefix.size() - 1, template_content);
            if (end_pos == std::string::npos || end_pos != type_name.size())
                return false;

            inner_type = normalise_cpp_type(template_content);
            return !inner_type.empty();
        }

        std::string rust_scalar_value_type_for_cpp_type(const std::string& cpp_type)
        {
            const auto type_name = normalise_cpp_type(cpp_type);

            if (type_name == "error_code")
                return "i32";
            if (type_name == "bool")
                return "bool";
            if (type_name == "int")
                return "i32";
            if (type_name == "uint64_t")
                return "u64";
            if (type_name == "size_t")
                return "usize";
            if (type_name == "std::string")
                return "String";
            if (type_name == "std::vector<uint8_t>" || type_name == "std::vector<unsigned char>")
                return "Vec<u8>";
            if (type_name == "std::vector<char>" || type_name == "std::vector<signed char>")
                return "Vec<i8>";

            return "";
        }

        std::string qualified_cpp_type_name(const class_entity& entity)
        {
            return get_full_name(entity, true, false, "::");
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

            if (!resolved_entity || resolved_entity->get_entity_type() != entity_type::STRUCT || resolved_entity->get_is_template())
                return nullptr;

            return resolved_entity.get();
        }

        bool is_supported_rust_struct_entity(
            const class_entity& current_scope,
            const class_entity& lib,
            const class_entity& struct_entity,
            std::set<std::string>& seen_structs);

        bool is_supported_rust_struct_field_type(
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& cpp_type,
            std::set<std::string>& seen_structs)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            if (!rust_scalar_value_type_for_cpp_type(type_name).empty())
                return true;

            std::string inner_type;
            if (extract_vector_value_type(type_name, inner_type))
            {
                if (!rust_scalar_value_type_for_cpp_type(inner_type).empty())
                    return true;
                const auto* inner_struct = resolve_non_template_struct_entity(current_scope, lib, inner_type);
                return inner_struct && is_supported_rust_struct_entity(current_scope, lib, *inner_struct, seen_structs);
            }

            const auto* struct_entity = resolve_non_template_struct_entity(current_scope, lib, type_name);
            return struct_entity && is_supported_rust_struct_entity(current_scope, lib, *struct_entity, seen_structs);
        }

        bool is_supported_rust_struct_entity(
            const class_entity& current_scope,
            const class_entity& lib,
            const class_entity& struct_entity,
            std::set<std::string>& seen_structs)
        {
            (void)current_scope;
            const auto struct_key = qualified_cpp_type_name(struct_entity);
            if (!seen_structs.insert(struct_key).second)
                return true;

            for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
            {
                if (!member || member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;
                auto func_entity = std::static_pointer_cast<function_entity>(member);
                if (func_entity->is_static())
                    continue;
                if (!is_supported_rust_struct_field_type(struct_entity, lib, func_entity->get_return_type(), seen_structs))
                    return false;
            }

            return true;
        }

        bool is_supported_rust_struct_type(
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& cpp_type)
        {
            const auto* struct_entity = resolve_non_template_struct_entity(current_scope, lib, cpp_type);
            if (!struct_entity)
                return false;

            std::set<std::string> seen_structs;
            return is_supported_rust_struct_entity(current_scope, lib, *struct_entity, seen_structs);
        }

        bool can_emit_rust_value_struct(
            const class_entity& type_entity,
            const class_entity& lib)
        {
            if (type_entity.get_is_template())
                return false;

            std::set<std::string> seen_structs;
            return is_supported_rust_struct_entity(type_entity, lib, type_entity, seen_structs);
        }

        std::string rust_struct_crate_path(
            const std::string& root_module_name,
            const class_entity& struct_entity)
        {
            return "crate::" + root_module_name + "::" + qualified_rust_module_path(struct_entity) + "::Value";
        }

        std::string rust_value_type_for_cpp_type_with_lib(
            const std::string& cpp_type,
            const class_entity& current_scope,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            const auto scalar_type = rust_scalar_value_type_for_cpp_type(type_name);
            if (!scalar_type.empty())
                return scalar_type;

            std::string inner_type;
            if (extract_vector_value_type(type_name, inner_type))
            {
                const auto inner_rust_type = rust_scalar_value_type_for_cpp_type(inner_type);
                if (!inner_rust_type.empty())
                    return "Vec<" + inner_rust_type + ">";
                const auto* inner_struct = resolve_non_template_struct_entity(current_scope, lib, inner_type);
                if (inner_struct && is_supported_rust_struct_type(current_scope, lib, inner_type))
                    return "Vec<" + rust_struct_crate_path(root_module_name, *inner_struct) + ">";
            }

            const auto* struct_entity = resolve_non_template_struct_entity(current_scope, lib, type_name);
            if (struct_entity && is_supported_rust_struct_type(current_scope, lib, type_name))
                return rust_struct_crate_path(root_module_name, *struct_entity);

            return "canopy_rpc::OpaqueValue";
        }

        struct rust_interface_generic
        {
            std::string parameter_name;
            std::string generic_name;
            std::string trait_path;
            bool is_optimistic = false;
            bool is_out = false;
        };

        struct rust_method_param
        {
            std::string rust_name;
            std::string rust_type;
            std::string generic_name;
            std::string associated_type_name;
            std::string associated_default_type;
            std::string trait_path;
            bool is_interface = false;
            bool is_optimistic = false;
            bool is_in = false;
            bool is_out = false;
        };

        std::vector<rust_method_param> analyse_rust_method_params(
            const class_entity& iface,
            const function_entity& function,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            std::vector<rust_method_param> result;
            size_t interface_index = 0;

            for (const auto& parameter : function.get_parameters())
            {
                rust_method_param param;
                param.rust_name = sanitize_identifier(parameter.get_name());
                param.is_in = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                param.is_out = is_out_param(parameter);

                bool is_optimistic = false;
                std::shared_ptr<class_entity> interface_entity;
                param.is_interface = is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity);
                param.is_optimistic = is_optimistic;

                if (param.is_interface)
                {
                    param.generic_name = uppercase_identifier(parameter.get_name()) + "Iface" + std::to_string(interface_index++);
                    param.associated_type_name = upper_camel_identifier(function.get_name()) + upper_camel_identifier(parameter.get_name())
                        + "Iface" + std::to_string(interface_index - 1);
                    param.associated_default_type = "crate::" + sanitize_identifier(root_module_name) + "::"
                        + qualified_rust_module_path(*interface_entity) + "::ProxySkeleton";
                    param.trait_path = "crate::" + sanitize_identifier(root_module_name) + "::" + qualified_rust_module_path(*interface_entity)
                        + "::Interface";
                    param.rust_type = is_optimistic
                                          ? "canopy_rpc::Optimistic<canopy_rpc::LocalProxy<" + param.generic_name + ">>"
                                          : "canopy_rpc::Shared<std::sync::Arc<" + param.generic_name + ">>";
                }
                else
                {
                    param.rust_type = rust_value_type_for_cpp_type_with_lib(parameter.get_type(), iface, lib, root_module_name);
                }

                result.push_back(std::move(param));
            }

            return result;
        }

        std::string generic_declaration_for_params(
            const std::vector<rust_method_param>& params,
            bool include_in,
            bool include_out)
        {
            std::vector<std::string> generics;
            for (const auto& param : params)
            {
                if (!param.is_interface)
                    continue;
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
            const std::vector<rust_method_param>& params,
            bool include_in,
            bool include_out)
        {
            std::vector<std::string> constraints;
            for (const auto& param : params)
            {
                if (!param.is_interface)
                    continue;
                if ((param.is_in && include_in) || (param.is_out && include_out))
                    constraints.push_back(param.generic_name + ": " + param.trait_path);
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

        std::string associated_type_args_for_params(
            const std::vector<rust_method_param>& params)
        {
            std::vector<std::string> args;
            for (const auto& param : params)
            {
                if (!param.is_interface)
                    continue;
                args.push_back("<Self as Interface>::" + param.associated_type_name);
            }

            if (args.empty())
                return "";

            std::string result = "<";
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (i)
                    result += ", ";
                result += args[i];
            }
            result += ">";
            return result;
        }

        void write_associated_type_impls(
            writer& output,
            const class_entity& iface,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            std::set<std::string> emitted_associated_types;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                const auto analysed_params = analyse_rust_method_params(iface, *function, lib, root_module_name);
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface)
                        continue;
                    if (!emitted_associated_types.insert(param.associated_type_name).second)
                        continue;
                    output("\ttype {} = {};", param.associated_type_name, param.associated_default_type);
                }
            }
        }

        std::string rust_method_signature(
            const class_entity& iface,
            const function_entity& function,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            std::vector<std::string> parameters;
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);

            std::string signature = "fn " + sanitize_identifier(function.get_name());
            signature += generic_declaration_for_params(analysed_params, true, true);

            signature += "(&self";
            for (const auto& param : analysed_params)
            {
                auto rust_type = param.rust_type;
                if (param.is_out)
                    rust_type = "&mut " + rust_type;

                signature += ", ";
                signature += param.rust_name + ": " + rust_type;
            }
            signature += ") -> ";
            signature += rust_value_type_for_cpp_type_with_lib(function.get_return_type(), iface, lib, root_module_name);
            signature += generic_where_clause_for_params(analysed_params, true, true);

            return signature;
        }

        void write_interface_match_helpers(
            writer& output,
            const class_entity& iface)
        {
            output("pub fn matches_interface_id(interface_id: canopy_rpc::InterfaceOrdinal) -> bool");
            output("{{");
            output("\tif interface_id == canopy_rpc::InterfaceOrdinal::new(ID_RPC_V3) || interface_id == canopy_rpc::InterfaceOrdinal::new(ID_RPC_V2)");
            output("\t{{");
            output("\t\treturn true;");
            output("\t}}");
            for (auto* base_class : iface.get_base_classes())
            {
                if (!base_class)
                    continue;
                output(
                    "\tif {}::matches_interface_id(interface_id)",
                    qualified_rust_module_path(*base_class));
                output("\t{{");
                output("\t\treturn true;");
                output("\t}}");
            }
            output("\tfalse");
            output("}}");
            output("");

            output(
                "pub fn method_metadata_for_interface(interface_id: canopy_rpc::InterfaceOrdinal) -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor]");
            output("{{");
            output("\tif interface_id == canopy_rpc::InterfaceOrdinal::new(ID_RPC_V3) || interface_id == canopy_rpc::InterfaceOrdinal::new(ID_RPC_V2)");
            output("\t{{");
            output("\t\treturn interface_binding::METHODS;");
            output("\t}}");
            for (auto* base_class : iface.get_base_classes())
            {
                if (!base_class)
                    continue;
                output(
                    "\tif {}::matches_interface_id(interface_id)",
                    qualified_rust_module_path(*base_class));
                output("\t{{");
                output(
                    "\t\treturn {}::method_metadata_for_interface(interface_id);",
                    qualified_rust_module_path(*base_class));
                output("\t}}");
            }
            output("\t&[]");
            output("}}");
            output("");
        }

        std::string rust_default_value_expression_for_type_name(const std::string& type_name)
        {
            if (type_name == "i32")
                return "canopy_rpc::INVALID_DATA()";
            if (type_name == "bool")
                return "false";
            if (type_name == "u64")
                return "0u64";
            if (type_name == "usize")
                return "0usize";
            if (type_name == "String")
                return "String::new()";
            if (type_name.rfind("Vec<", 0) == 0)
                return "Vec::new()";
            if (type_name.rfind("crate::", 0) == 0)
                return "Default::default()";

            return "canopy_rpc::OpaqueValue";
        }

        std::string rust_default_value_expression_for_param(const rust_method_param& param)
        {
            if (param.is_interface)
                return param.is_optimistic ? "canopy_rpc::Optimistic::null()" : "canopy_rpc::Shared::null()";

            return rust_default_value_expression_for_type_name(param.rust_type);
        }

        std::string rust_dispatch_type_for_param(const rust_method_param& param)
        {
            if (param.is_interface)
                return "canopy_rpc::RemoteObject";

            return param.rust_type;
        }

        void write_generated_skeleton_method_body(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const std::string& delegate_trait,
            const std::string& delegate_method_prefix,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto method_module = sanitize_identifier(function.get_name());

            output("{} {{", rust_method_signature(iface, function, lib, root_module_name));
            output("\tlet _request = interface_binding::{}::Request::from_call(", method_module);
            {
                bool first = true;
                for (const auto& param : analysed_params)
                {
                    if (!param.is_in)
                        continue;
                    if (!first)
                        output.raw(", ");
                    first = false;
                    output.raw("{}", param.rust_name);
                }
            }
            output.raw(");\n");
            if (delegate_trait == "Proxy")
            {
                output("\tlet _response = self.{}_{}(_request);", delegate_method_prefix, method_module);
            }
            else
            {
                output(
                    "\tlet _response = <Self as {}>::{}_{}(_request);",
                    delegate_trait,
                    delegate_method_prefix,
                    method_module);
            }
            std::string apply_line = "\t_response.apply_to_call(";
            bool first = true;
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                if (!first)
                    apply_line += ", ";
                first = false;
                apply_line += param.rust_name;
            }
            apply_line += ")";
            output("{}", apply_line);
            output("}}");
        }

        void write_generated_delegate_method(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const std::string& method_prefix,
            const std::string& root_module_name,
            const class_entity& lib)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto method_module = sanitize_identifier(function.get_name());
            const auto all_generics = generic_declaration_for_params(analysed_params, true, true);
            const auto request_generics = generic_declaration_for_params(analysed_params, true, false);
            const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
            auto where_clause = generic_where_clause_for_params(analysed_params, true, true);
            const auto protobuf_module_path = "crate::" + sanitize_identifier(root_module_name) + "_protobuf::"
                                            + qualified_rust_module_path(iface) + "::interface_binding::"
                                            + method_module;
            if (where_clause.empty())
                where_clause = " where Self: Sized";
            else
                where_clause += ", Self: Sized";

            const bool is_proxy_method = method_prefix == "proxy_call";
            if (is_proxy_method)
            {
                output(
                    "fn {}_{}{}(&self, request: interface_binding::{}::Request{}) -> interface_binding::{}::Response{}{}",
                    method_prefix,
                    method_module,
                    all_generics,
                    method_module,
                    request_generics,
                    method_module,
                    response_generics,
                    where_clause);
            }
            else
            {
                output(
                    "fn {}_{}{}(request: interface_binding::{}::Request{}) -> interface_binding::{}::Response{}{}",
                    method_prefix,
                    method_module,
                    all_generics,
                    method_module,
                    request_generics,
                    method_module,
                    response_generics,
                    where_clause);
            }
            output("{{");
            if (is_proxy_method)
            {
                output("\tlet protocol_version = self");
                output("\t\t.proxy_caller()");
                output("\t\t.map(|transport| transport.call_context().protocol_version)");
                output("\t\t.unwrap_or(canopy_rpc::get_version());");
                output(
                    "\treturn canopy_rpc::serialization::protobuf::call_generated_proxy_method::<{}::ProtobufCodec{}, _, _>(",
                    protobuf_module_path,
                    all_generics);
                output("\t\tself.proxy_caller(),");
                output(
                    "\t\tcanopy_rpc::InterfaceOrdinal::new(<Self as Interface>::get_id(protocol_version)),");
                output(
                    "\t\tcanopy_rpc::Method::new(methods::{}),",
                    uppercase_identifier(function.get_name()));
                output("\t\t&request,");
                output(
                    "\t\t|error_code| interface_binding::{}::Response::from_error_code(error_code),",
                    method_module);
                output(
                    "\t\t|| interface_binding::{}::Response::from_error_code(canopy_rpc::PROXY_DESERIALISATION_ERROR()),",
                    method_module);
                output("\t);");
            }
            else
            {
                output("\tlet _ = request;");
                output(
                    "\tinterface_binding::{}::Response::from_error_code(canopy_rpc::STUB_DESERIALISATION_ERROR())",
                    method_module);
            }
            output("}}");
            output("");
        }

        void write_request_response_shapes(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto return_type = rust_value_type_for_cpp_type_with_lib(function.get_return_type(), iface, lib, root_module_name);
            const bool has_return_value = normalise_cpp_type(function.get_return_type()) != "void" && !function.get_return_type().empty();

            const auto request_generics = generic_declaration_for_params(analysed_params, true, false);
            const auto request_where = generic_where_clause_for_params(analysed_params, true, false);
            output("pub struct Request{}{}{{", request_generics, request_where);
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;
                output("\tpub {}: {},", param.rust_name, param.rust_type);
            }
            output("}}");
            output("");

            output("impl{} Request{}{}{{", request_generics, request_generics, request_where);
            output("\tpub fn from_call(");
            {
                bool first = true;
                for (const auto& param : analysed_params)
                {
                    if (!param.is_in)
                        continue;
                    if (!first)
                        output.raw(", ");
                    first = false;
                    output.raw("{}: {}", param.rust_name, param.rust_type);
                }
            }
            output.raw(") -> Self\n");
            output("\t{{");
            output("\t\tSelf{{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;
                output("\t\t\t{}: {},", param.rust_name, param.rust_name);
            }
            output("\t\t}}");
            output("\t}}");
            output("");
            output("}}");
            output("");

            output("#[doc(hidden)]");
            output("pub struct DispatchRequest{{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;
                output("\tpub {}: {},", param.rust_name, rust_dispatch_type_for_param(param));
            }
            output("}}");
            output("");

            const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
            const auto response_where = generic_where_clause_for_params(analysed_params, false, true);
            output("pub struct Response{}{}{{", response_generics, response_where);
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\tpub {}: {},", param.rust_name, param.rust_type);
            }
            if (has_return_value)
                output("\tpub return_value: {},", return_type);
            output("}}");
            output("");

            output("impl{} Response{}{}{{", response_generics, response_generics, response_where);
            output("\tpub fn from_error_code(error_code: i32) -> Self");
            output("\t{{");
            output("\t\tSelf{{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\t\t\t{}: {},", param.rust_name, rust_default_value_expression_for_param(param));
            }
            if (has_return_value)
            {
                if (return_type == "i32")
                    output("\t\t\treturn_value: error_code,");
                else
                    output(
                        "\t\t\treturn_value: {},",
                        rust_default_value_expression_for_type_name(return_type));
            }
            output("\t\t}}");
            output("\t}}");
            output("");
            output("\tpub fn apply_to_call(self");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output.raw(", {}: &mut {}", param.rust_name, param.rust_type);
            }
            if (has_return_value)
                output.raw(") -> {}\n", return_type);
            else
                output.raw(")\n");
            output("\t{{");
            output("\t\tlet Self{{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\t\t\t{}: {}_value,", param.rust_name, param.rust_name);
            }
            if (has_return_value)
                output("\t\t\treturn_value,");
            output("\t\t}} = self;");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\t\t*{} = {}_value;", param.rust_name, param.rust_name);
            }
            if (has_return_value)
                output("\t\treturn_value");
            output("\t}}");
            output("}}");
            output("");

            output("#[doc(hidden)]");
            output("pub struct DispatchResponse{{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\tpub {}: {},", param.rust_name, rust_dispatch_type_for_param(param));
            }
            if (has_return_value)
                output("\tpub return_value: {},", return_type);
            output("}}");
            output("");
        }

        const char* rust_direction_name(
            bool is_in,
            bool is_out)
        {
            if (is_out)
                return "Out";
            if (is_in || !is_out)
                return "In";
            return "In";
        }

        void write_param_binding_helpers(
            writer& output,
            const class_entity& iface,
            const function_entity& function)
        {
            for (const auto& parameter : function.get_parameters())
            {
                bool is_optimistic = false;
                std::shared_ptr<class_entity> interface_entity;
                if (!is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                    continue;

                const auto param_name = sanitize_identifier(parameter.get_name());
                const auto interface_name = interface_entity ? interface_entity->get_name() : "";

                if (is_optimistic)
                {
                    output(
                        "pub fn bind_{}_incoming<T, LocalLookup, RemoteBind>("
                        "service_zone: &canopy_rpc::Zone, "
                        "encap: &canopy_rpc::RemoteObject, "
                        "lookup_local: LocalLookup, "
                        "bind_remote: RemoteBind) -> canopy_rpc::InterfaceBindResult<canopy_rpc::LocalProxy<T>>",
                        param_name);
                    output("where");
                    output("\tT: canopy_rpc::CreateLocalProxy,");
                    output("\tLocalLookup: FnOnce(canopy_rpc::Object) -> Result<std::sync::Arc<T>, i32>,");
                    output(
                        "\tRemoteBind: FnOnce(&canopy_rpc::RemoteObject) -> canopy_rpc::InterfaceBindResult<canopy_rpc::LocalProxy<T>>,");
                    output("{{");
                    output("\tcanopy_rpc::bind_incoming_optimistic(service_zone, encap, lookup_local, bind_remote)");
                    output("}}");
                    output("");
                    output(
                        "pub fn bind_{}_incoming_local<T>("
                        "service: &canopy_rpc::Service, "
                        "encap: &canopy_rpc::RemoteObject) -> "
                        "canopy_rpc::InterfaceBindResult<canopy_rpc::LocalProxy<T>>",
                        param_name);
                    output("where");
                    output("\tT: canopy_rpc::GeneratedRustInterface,");
                    output("{{");
                    output("\tservice.bind_incoming_optimistic_interface::<T>(encap)");
                    output("}}");
                }
                else
                {
                    output(
                        "pub fn bind_{}_incoming<T, LocalLookup, RemoteBind>("
                        "service_zone: &canopy_rpc::Zone, "
                        "encap: &canopy_rpc::RemoteObject, "
                        "lookup_local: LocalLookup, "
                        "bind_remote: RemoteBind) -> canopy_rpc::InterfaceBindResult<T>",
                        param_name);
                    output("where");
                    output("\tLocalLookup: FnOnce(canopy_rpc::Object) -> Result<T, i32>,");
                    output("\tRemoteBind: FnOnce(&canopy_rpc::RemoteObject) -> canopy_rpc::InterfaceBindResult<T>,");
                    output("{{");
                    output("\tcanopy_rpc::bind_incoming_shared(service_zone, encap, lookup_local, bind_remote)");
                    output("}}");
                    output("");
                    output(
                        "pub fn bind_{}_incoming_local<T>("
                        "service: &canopy_rpc::Service, "
                        "encap: &canopy_rpc::RemoteObject) -> "
                        "canopy_rpc::InterfaceBindResult<std::sync::Arc<T>>",
                        param_name);
                    output("where");
                    output("\tT: canopy_rpc::GeneratedRustInterface,");
                    output("{{");
                    output("\tservice.bind_incoming_shared_interface::<T>(encap)");
                    output("}}");
                }
                output("");

                output(
                    "pub fn bind_{}_outgoing<T, Stub, BindLocal, BindRemote>("
                    "iface: &canopy_rpc::BoundInterface<T>, "
                    "bind_local: BindLocal, "
                    "bind_remote: BindRemote) -> canopy_rpc::RemoteObjectBindResult<Stub>",
                    param_name);
                output("where");
                output("\tT: canopy_rpc::BindableInterfaceValue,");
                output(
                    "\tBindLocal: FnOnce(&T, canopy_rpc::InterfacePointerKind) -> canopy_rpc::RemoteObjectBindResult<Stub>,");
                output(
                    "\tBindRemote: FnOnce(&T, canopy_rpc::InterfacePointerKind) -> canopy_rpc::RemoteObjectBindResult<Stub>,");
                output("{{");
                output(
                    "\tcanopy_rpc::bind_outgoing_interface("
                    "iface, canopy_rpc::InterfacePointerKind::{}, bind_local, bind_remote)",
                    is_optimistic ? "Optimistic" : "Shared");
                output("}}");
                output("");
                output(
                    "pub fn bind_{}_outgoing_local<T>("
                    "service: &canopy_rpc::Service, "
                    "caller_zone_id: canopy_rpc::CallerZone, "
                    "iface: &{}) -> "
                    "canopy_rpc::RemoteObjectBindResult<std::sync::Arc<std::sync::Mutex<canopy_rpc::ObjectStub>>>",
                    param_name,
                    is_optimistic
                        ? "canopy_rpc::Optimistic<canopy_rpc::LocalProxy<T>>"
                        : "canopy_rpc::Shared<std::sync::Arc<T>>");
                output("where");
                output("\tT: canopy_rpc::GeneratedRustInterface,");
                output("{{");
                if (is_optimistic)
                {
                    output("\tservice.bind_outgoing_local_optimistic_interface(caller_zone_id, iface.as_inner())");
                }
                else
                {
                    output(
                        "\tservice.bind_outgoing_local_interface(caller_zone_id, iface.as_inner(), canopy_rpc::InterfacePointerKind::Shared)");
                }
                output("}}");
                output("");

                output("pub const {}_INTERFACE_NAME: &str = \"{}\";", uppercase_identifier(param_name), interface_name);
                output("");
            }
        }

        void write_method_local_binding_helpers(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
            const auto response_where = generic_where_clause_for_params(analysed_params, false, true);
            const auto all_generics = generic_declaration_for_params(analysed_params, true, true);
            auto all_where = generic_where_clause_for_params(analysed_params, true, true);

            bool has_interface_in = false;
            bool has_interface_out = false;
            for (const auto& param : analysed_params)
            {
                if (!param.is_interface)
                    continue;
                has_interface_in = has_interface_in || param.is_in;
                has_interface_out = has_interface_out || param.is_out;
            }

            if (has_interface_in)
            {
                const auto incoming_generics = generic_declaration_for_params(analysed_params, true, false);
                const auto incoming_where = generic_where_clause_for_params(analysed_params, true, false);

                output("pub struct IncomingLocalBindings{}{}", incoming_generics, incoming_where);
                output("{{");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;

                    output(
                        "\tpub {}: {},",
                        param.rust_name,
                        param.is_optimistic
                            ? "canopy_rpc::InterfaceBindResult<canopy_rpc::LocalProxy<" + param.generic_name + ">>"
                            : "canopy_rpc::InterfaceBindResult<std::sync::Arc<" + param.generic_name + ">>");
                }
                output("}}");
                output("");

                output("impl{} IncomingLocalBindings{}{}", incoming_generics, incoming_generics, incoming_where);
                output("{{");
                output("\tpub fn bind(service: &canopy_rpc::Service");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;
                    output.raw(", {}: &canopy_rpc::RemoteObject", param.rust_name);
                }
                output.raw(") -> Self\n");
                output("\t{{");
                output("\t\tSelf {{");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;
                    output(
                        "\t\t\t{}: bind_{}_incoming_local::<{}>(service, {}),",
                        param.rust_name,
                        param.rust_name,
                        param.generic_name,
                        param.rust_name);
                }
                output("\t\t}}");
                output("\t}}");
                output("}}");
                output("");

                output(
                    "pub fn bind_incoming_local{}(service: &canopy_rpc::Service",
                    incoming_generics);
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;
                    output.raw(", {}: &canopy_rpc::RemoteObject", param.rust_name);
                }
                output.raw(") -> IncomingLocalBindings{}\n", incoming_generics);
                if (!incoming_where.empty())
                    output("{}", incoming_where);
                output("{{");
                output("\tIncomingLocalBindings::bind(service");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;
                    output.raw(", {}", param.rust_name);
                }
                output.raw(")\n");
                output("}}");
                output("");
            }

            if (has_interface_out)
            {
                const auto outgoing_generics = generic_declaration_for_params(analysed_params, false, true);
                const auto outgoing_where = generic_where_clause_for_params(analysed_params, false, true);

                output("pub struct OutgoingLocalBindings{}{}", outgoing_generics, outgoing_where);
                output("{{");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;

                    output(
                        "\tpub {}: canopy_rpc::RemoteObjectBindResult<std::sync::Arc<std::sync::Mutex<canopy_rpc::ObjectStub>>>,",
                        param.rust_name);
                }
                output("}}");
                output("");

                output("impl{} OutgoingLocalBindings{}{}", outgoing_generics, outgoing_generics, outgoing_where);
                output("{{");
                output("\tpub fn bind(service: &canopy_rpc::Service, caller_zone_id: canopy_rpc::CallerZone");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;
                    output.raw(", {}: &{}", param.rust_name, param.rust_type);
                }
                output.raw(") -> Self\n");
                output("\t{{");
                output("\t\tSelf {{");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;
                    output(
                        "\t\t\t{}: bind_{}_outgoing_local::<{}>(service, caller_zone_id.clone(), {}),",
                        param.rust_name,
                        param.rust_name,
                        param.generic_name,
                        param.rust_name);
                }
                output("\t\t}}");
                output("\t}}");
                output("}}");
                output("");

                output(
                    "pub fn bind_outgoing_local{}(service: &canopy_rpc::Service, caller_zone_id: canopy_rpc::CallerZone",
                    outgoing_generics);
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;
                    output.raw(", {}: &{}", param.rust_name, param.rust_type);
                }
                output.raw(") -> OutgoingLocalBindings{}\n", outgoing_generics);
                if (!outgoing_where.empty())
                    output("{}", outgoing_where);
                output("{{");
                output("\tOutgoingLocalBindings::bind(service, caller_zone_id");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;
                    output.raw(", {}", param.rust_name);
                }
                output.raw(")\n");
                output("}}");
                output("");
            }

            output("pub struct LocalCallResult{}{}", all_generics, all_where);
            output("{{");
            if (has_interface_in)
                output("\tpub incoming: IncomingLocalBindings{},", generic_declaration_for_params(analysed_params, true, false));
            output("\tpub response: Response{},", response_generics);
            if (has_interface_out)
                output("\tpub outgoing: OutgoingLocalBindings{},", generic_declaration_for_params(analysed_params, false, true));
            output("}}");
            output("");

            if (all_where.empty())
                all_where = " where Impl: super::super::Interface";
            else
                all_where += ", Impl: super::super::Interface";

            output(
                "pub fn call_local<Impl{}>(_service: &canopy_rpc::Service, _caller_zone_id: canopy_rpc::CallerZone, implementation: &Impl",
                all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2));
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;

                if (param.is_interface)
                    output.raw(", {}: &canopy_rpc::RemoteObject", param.rust_name);
                else
                    output.raw(", {}: {}", param.rust_name, param.rust_type);
            }
            output.raw(") -> LocalCallResult{}\n", all_generics);
            output("{}", all_where);
            output("{{");

            if (has_interface_in)
            {
                output("\tlet incoming = bind_incoming_local(_service");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;
                    output.raw(", {}", param.rust_name);
                }
                output.raw(");\n");

                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;

                    output("\tif canopy_rpc::is_critical(incoming.{}.error_code)", param.rust_name);
                    output("\t{{");
                    output(
                        "\t\tlet response: Response{} = Response::from_error_code(incoming.{}.error_code);",
                        response_generics,
                        param.rust_name);
                    if (has_interface_out)
                    {
                        output("\t\tlet outgoing = bind_outgoing_local(_service, _caller_zone_id.clone()");
                        for (const auto& out_param : analysed_params)
                        {
                            if (!out_param.is_interface || !out_param.is_out)
                                continue;
                            output.raw(", &response.{}", out_param.rust_name);
                        }
                        output.raw(");\n");
                    }
                    output("\t\treturn LocalCallResult {{");
                    if (has_interface_in)
                        output("\t\t\tincoming,");
                    output("\t\t\tresponse,");
                    if (has_interface_out)
                        output("\t\t\toutgoing,");
                    output("\t\t}};");
                    output("\t}}");
                }

                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_in)
                        continue;

                    output(
                        "\tlet {} = {}(incoming.{}.iface.clone());",
                        param.rust_name,
                        param.is_optimistic ? "canopy_rpc::Optimistic" : "canopy_rpc::Shared",
                        param.rust_name);
                }
            }

            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\tlet mut {} = {};", param.rust_name, rust_default_value_expression_for_param(param));
            }

            const auto return_type = rust_value_type_for_cpp_type_with_lib(function.get_return_type(), iface, lib, root_module_name);
            const bool has_return_value = normalise_cpp_type(function.get_return_type()) != "void" && !function.get_return_type().empty();
            if (has_return_value)
            {
                output("\tlet return_value = implementation.{}(", sanitize_identifier(function.get_name()));
            }
            else
            {
                output("\timplementation.{}(", sanitize_identifier(function.get_name()));
            }
            {
                bool first = true;
                for (const auto& param : analysed_params)
                {
                    if (!first)
                        output.raw(", ");
                    first = false;

                    if (param.is_out)
                        output.raw("&mut {}", param.rust_name);
                    else
                        output.raw("{}", param.rust_name);
                }
            }
            output.raw(");\n");

            output("\tlet response = Response {{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\t\t{}: {},", param.rust_name, param.rust_name);
            }
            if (has_return_value)
                output("\t\treturn_value,");
            output("\t}};");

            if (has_interface_out)
            {
                output("\tlet outgoing = bind_outgoing_local(_service, _caller_zone_id");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;
                    output.raw(", &response.{}", param.rust_name);
                }
                output.raw(");\n");
            }

            output("\tLocalCallResult {{");
            if (has_interface_in)
                output("\t\tincoming,");
            output("\t\tresponse,");
            if (has_interface_out)
                output("\t\toutgoing,");
            output("\t}}");
            output("}}");
            output("");
        }

        void write_method_generated_dispatch_helper(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const std::string& root_module_name,
            const class_entity& lib)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto all_generics = generic_declaration_for_params(analysed_params, true, true);
            auto all_where = generic_where_clause_for_params(analysed_params, true, true);
            if (all_where.empty())
                all_where = " where Impl: super::super::Interface";
            else
                all_where += ", Impl: super::super::Interface";
            const auto protobuf_module_path = "crate::" + sanitize_identifier(root_module_name) + "_protobuf::"
                                            + qualified_rust_module_path(iface) + "::interface_binding::"
                                            + sanitize_identifier(function.get_name());

            output("#[doc(hidden)]");
            output(
                "pub fn dispatch_generated<Impl{}>(implementation: &Impl, context: &canopy_rpc::DispatchContext, params: canopy_rpc::SendParams) -> canopy_rpc::SendResult",
                all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2));
            output("{}", all_where);
            output("{{");
            output("\tmatch params.encoding_type");
            output("\t{{");
            output(
                "\t\tcanopy_rpc::Encoding::ProtocolBuffers => {}::dispatch_generated::<Impl{}>(implementation, context, params),",
                protobuf_module_path,
                all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2));
            output("\t\t_ => canopy_rpc::SendResult::new(canopy_rpc::INCOMPATIBLE_SERIALISATION(), vec![], vec![]),");
            output("\t}}");
            output("}}");
            output("");
        }

        void write_method_decoded_dispatch_helper(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto all_generics = generic_declaration_for_params(analysed_params, true, true);
            const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
            const auto request_generics = generic_declaration_for_params(analysed_params, true, false);
            auto all_where = generic_where_clause_for_params(analysed_params, true, true);
            if (all_where.empty())
                all_where = " where Impl: super::super::Interface";
            else
                all_where += ", Impl: super::super::Interface";
            const auto return_type = rust_value_type_for_cpp_type_with_lib(function.get_return_type(), iface, lib, root_module_name);
            const bool has_return_value = normalise_cpp_type(function.get_return_type()) != "void" && !function.get_return_type().empty();

            output("#[doc(hidden)]");
            output(
                "pub fn dispatch_decoded_request<Impl{}>(implementation: &Impl, context: &canopy_rpc::DispatchContext, request: Request{}) -> Result<Response{}, i32>",
                all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2),
                request_generics,
                response_generics);
            output("{}", all_where);
            output("{{");
            output("\tlet _ = context;");
            output("\tlet Request {{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;
                output("\t\t{},", param.rust_name);
            }
            output("\t}} = request;");

            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\tlet mut {} = {};", param.rust_name, rust_default_value_expression_for_param(param));
            }

            if (has_return_value)
                output("\tlet return_value = implementation.{}(", sanitize_identifier(function.get_name()));
            else
                output("\timplementation.{}(", sanitize_identifier(function.get_name()));

            {
                bool first = true;
                for (const auto& param : analysed_params)
                {
                    if (!first)
                        output.raw(", ");
                    first = false;

                    if (param.is_out)
                        output.raw("&mut {}", param.rust_name);
                    else
                        output.raw("{}", param.rust_name);
                }
            }
            output.raw(");\n");

            output("\tOk(Response {{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                output("\t\t{}: {},", param.rust_name, param.rust_name);
            }
            if (has_return_value)
                output("\t\treturn_value,");
            output("\t}})");
            output("}}");
            output("");
        }

        void write_method_stub_dispatch_helper(
            writer& output,
            const class_entity& iface,
            const function_entity& function,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            const auto analysed_params = analyse_rust_method_params(iface, function, lib, root_module_name);
            const auto all_generics = generic_declaration_for_params(analysed_params, true, true);
            const auto request_generics = generic_declaration_for_params(analysed_params, true, false);
            const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
            auto all_where = generic_where_clause_for_params(analysed_params, true, true);
            if (all_where.empty())
                all_where = " where Impl: super::super::Interface";
            else
                all_where += ", Impl: super::super::Interface";
            const bool has_return_value = normalise_cpp_type(function.get_return_type()) != "void" && !function.get_return_type().empty();

            bool has_interface_params = false;
            bool has_interface_out = false;
            for (const auto& param : analysed_params)
            {
                if (!param.is_interface)
                    continue;
                has_interface_params = true;
                has_interface_out = has_interface_out || param.is_out;
            }

            output("#[doc(hidden)]");
            output(
                "pub fn dispatch_stub_request<Impl{}>(implementation: &Impl, context: &canopy_rpc::DispatchContext, request: DispatchRequest) -> Result<DispatchResponse, i32>",
                all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2));
            output("{}", all_where);
            output("{{");

            if (!has_interface_params)
            {
                output("\tlet _ = context;");
                output("\tlet DispatchRequest {{");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_in)
                        continue;
                    output("\t\t{},", param.rust_name);
                }
                output("\t}} = request;");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_out)
                        continue;
                    output("\tlet mut {} = {};", param.rust_name, rust_default_value_expression_for_param(param));
                }
                if (has_return_value)
                    output("\tlet return_value = implementation.{}(", sanitize_identifier(function.get_name()));
                else
                    output("\timplementation.{}(", sanitize_identifier(function.get_name()));
                {
                    bool first = true;
                    for (const auto& param : analysed_params)
                    {
                        if (!first)
                            output.raw(", ");
                        first = false;
                        if (param.is_out)
                            output.raw("&mut {}", param.rust_name);
                        else
                            output.raw("{}", param.rust_name);
                    }
                }
                output.raw(");\n");
                output("\tOk(DispatchResponse {{");
                for (const auto& param : analysed_params)
                {
                    if (!param.is_out)
                        continue;
                    output("\t\t{}: {},", param.rust_name, param.rust_name);
                }
                if (has_return_value)
                    output("\t\treturn_value,");
                output("\t}})");
                output("}}");
                output("");
                return;
            }

            output("\tlet DispatchRequest {{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;
                output("\t\t{},", param.rust_name);
            }
            output("\t}} = request;");
            output("\tlet Some(service) = context.current_service() else");
            output("\t{{");
            output("\t\treturn Err(canopy_rpc::INVALID_DATA());");
            output("\t}};");
            output("\tlet result = call_local::<Impl{}>(service, context.caller_zone_id.clone(), implementation", all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2));
            for (const auto& param : analysed_params)
            {
                if (!param.is_in)
                    continue;
                if (param.is_interface)
                    output.raw(", &{}", param.rust_name);
                else
                    output.raw(", {}", param.rust_name);
            }
            output.raw(");\n");

            if (has_interface_out)
            {
                for (const auto& param : analysed_params)
                {
                    if (!param.is_interface || !param.is_out)
                        continue;
                    output("\tif canopy_rpc::is_critical(result.outgoing.{}.error_code)", param.rust_name);
                    output("\t{{");
                    output("\t\treturn Err(result.outgoing.{}.error_code);", param.rust_name);
                    output("\t}}");
                }
            }

            output("\tOk(DispatchResponse {{");
            for (const auto& param : analysed_params)
            {
                if (!param.is_out)
                    continue;
                if (param.is_interface)
                    output("\t\t{}: result.outgoing.{}.descriptor,", param.rust_name, param.rust_name);
                else
                    output("\t\t{}: result.response.{},", param.rust_name, param.rust_name);
            }
            if (has_return_value)
                output("\t\treturn_value: result.response.return_value,");
            output("\t}})");
            output("}}");
            output("");
        }

        void write_method_binding_metadata(
            writer& output,
            const class_entity& iface,
            const std::string& root_module_name,
            const class_entity& lib)
        {
            output("pub mod interface_binding");
            output("{{");
            output("pub struct BindingMetadata;");
            output("");

            int method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                output("pub mod {}", sanitize_identifier(function->get_name()));
                output("{{");
                output("pub const INTERFACE_PARAMS: &[canopy_rpc::GeneratedInterfaceParamDescriptor] = &[");

                for (const auto& parameter : function->get_parameters())
                {
                    bool is_optimistic = false;
                    std::shared_ptr<class_entity> interface_entity;
                    if (!is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                        continue;

                    output(
                        "\tcanopy_rpc::GeneratedInterfaceParamDescriptor {{ name: \"{}\", interface_name: \"{}\", pointer_kind: "
                        "canopy_rpc::InterfacePointerKind::{}, direction: canopy_rpc::ParameterDirection::{} }},",
                        parameter.get_name(),
                        interface_entity ? interface_entity->get_name() : "",
                        is_optimistic ? "Optimistic" : "Shared",
                        rust_direction_name(is_in_param(parameter), is_out_param(parameter)));
                }

                output("];");
                output("");
                write_request_response_shapes(output, iface, *function, lib, root_module_name);
                write_param_binding_helpers(output, iface, *function);
                write_method_local_binding_helpers(output, iface, *function, lib, root_module_name);
                write_method_decoded_dispatch_helper(output, iface, *function, lib, root_module_name);
                write_method_stub_dispatch_helper(output, iface, *function, lib, root_module_name);
                write_method_generated_dispatch_helper(output, iface, *function, root_module_name, lib);
                output("}}");
                method_count++;
            }

            output("pub const METHODS: &[canopy_rpc::GeneratedMethodBindingDescriptor] = &[");
            method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto module_name = sanitize_identifier(function->get_name());
                output(
                    "\tcanopy_rpc::GeneratedMethodBindingDescriptor {{ method_name: \"{}\", method_id: {}u64, interface_params: "
                    "{}::INTERFACE_PARAMS "
                    "}} ,",
                    function->get_name(),
                    method_count,
                    module_name);
                method_count++;
            }
            output("];");
            output("");
            output("impl canopy_rpc::GeneratedInterfaceBindingMetadata for BindingMetadata");
            output("{{");
            output("\tfn interface_name() -> &'static str");
            output("\t{{");
            output("\t\t\"{}\"", iface.get_name());
            output("\t}}");
            output("");
            output("\tfn id_rpc_v2() -> u64");
            output("\t{{");
            output("\t\tsuper::ID_RPC_V2");
            output("\t}}");
            output("");
            output("\tfn id_rpc_v3() -> u64");
            output("\t{{");
            output("\t\tsuper::ID_RPC_V3");
            output("\t}}");
            output("");
            output("\tfn methods() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor]");
            output("\t{{");
            output("\t\tMETHODS");
            output("\t}}");
            output("}}");
            output("");
            output("pub fn by_method_id(method_id: u64) -> Option<&'static canopy_rpc::GeneratedMethodBindingDescriptor>");
            output("{{");
            output("\t<BindingMetadata as canopy_rpc::GeneratedInterfaceBindingMetadata>::by_method_id(method_id)");
            output("}}");
            output("}}");
        }

        bool is_different(
            const std::stringstream& stream,
            const std::string& data)
        {
            return stream.str() != data;
        }

        void write_interface(
            writer& output,
            const class_entity& iface,
            const std::string& root_module_name,
            const class_entity& lib)
        {
            output("pub mod {}", sanitize_identifier(iface.get_name()));
            output("{{");
            output("pub const NAME: &str = \"{}\";", iface.get_name());

            for (const auto& version : protocol_versions)
            {
                output(
                    "pub const ID_RPC_{}: u64 = {}u64;",
                    version.name,
                    fingerprint::generate(iface, {}, nullptr, version.value));
            }

            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() == entity_type::RUSTQUOTE)
                {
                    if (!function->is_in_import())
                        output.write_buffer(function->get_name());
                }
            }

            std::string trait_bases = "canopy_rpc::GeneratedRustInterface";
            for (auto* base_class : iface.get_base_classes())
            {
                if (!base_class)
                    continue;
                trait_bases += " + " + qualified_rust_module_path(*base_class) + "::Interface";
            }

            output("pub trait Interface: {}", trait_bases);
            output("{{");
            output("fn interface_name() -> &'static str where Self: Sized");
            output("{{");
            output("\tNAME");
            output("}}");
            output("");
            output("fn get_id(rpc_version: u64) -> u64 where Self: Sized");
            output("{{");
            output("\tif rpc_version >= 3u64");
            output("\t{{");
            output("\t\tID_RPC_V3");
            output("\t}}");
            output("\telse if rpc_version >= 2u64");
            output("\t{{");
            output("\t\tID_RPC_V2");
            output("\t}}");
            output("\telse");
            output("\t{{");
            output("\t\t0u64");
            output("\t}}");
            output("}}");
            output("");
            output("fn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor] where Self: Sized");
            output("{{");
            output("\tinterface_binding::METHODS");
            output("}}");
            output("");
            {
                std::set<std::string> emitted_associated_types;
                for (const auto& function : iface.get_functions())
                {
                    if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                        continue;
                    const auto analysed_params = analyse_rust_method_params(iface, *function, lib, root_module_name);
                    for (const auto& param : analysed_params)
                    {
                        if (!param.is_interface)
                            continue;
                        if (!emitted_associated_types.insert(param.associated_type_name).second)
                            continue;
                        output("type {}: {};", param.associated_type_name, param.trait_path);
                    }
                }
                if (!emitted_associated_types.empty())
                    output("");
            }
            output("#[doc(hidden)]");
            output(
                "fn __rpc_dispatch_generated(&self, context: &canopy_rpc::DispatchContext, params: canopy_rpc::SendParams) -> canopy_rpc::SendResult where Self: Sized");
            output("{{");
            output("\tif params.interface_id == canopy_rpc::InterfaceOrdinal::new(ID_RPC_V3) || params.interface_id == canopy_rpc::InterfaceOrdinal::new(ID_RPC_V2)");
            output("\t{{");
            output("\t\tmatch params.method_id.get_val()");
            output("\t\t{{");
            int dispatch_method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto analysed_params = analyse_rust_method_params(iface, *function, lib, root_module_name);
                bool has_interface_params = false;
                for (const auto& param : analysed_params)
                {
                    if (param.is_interface)
                    {
                        has_interface_params = true;
                        break;
                    }
                }

                output(
                    "\t\t\t{}u64 => return interface_binding::{}::dispatch_generated::<Self{}>(self, context, params),",
                    dispatch_method_count,
                    sanitize_identifier(function->get_name()),
                    has_interface_params
                        ? ", " + associated_type_args_for_params(analysed_params).substr(1, associated_type_args_for_params(analysed_params).size() - 2)
                        : "");
                dispatch_method_count++;
            }
            output("\t\t\t_ => return canopy_rpc::SendResult::new(canopy_rpc::INVALID_METHOD_ID(), vec![], vec![]),");
            output("\t\t}}");
            output("\t}}");
            for (auto* base_class : iface.get_base_classes())
            {
                if (!base_class)
                    continue;
                output(
                    "\tif {}::matches_interface_id(params.interface_id)",
                    qualified_rust_module_path(*base_class));
                output("\t{{");
                output(
                    "\t\treturn <Self as {}::Interface>::__rpc_dispatch_generated(self, context, params);",
                    qualified_rust_module_path(*base_class));
                output("\t}}");
            }
            output("\tcanopy_rpc::SendResult::new(canopy_rpc::INVALID_INTERFACE_ID(), vec![], vec![])");
            output("}}");
            output("");

            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                output("{};", rust_method_signature(iface, *function, lib, root_module_name));
            }
            output("}}");
            output("");

            write_interface_match_helpers(output, iface);

            output("pub struct RpcObjectAdapter;");
            output("");
            output("impl<T> canopy_rpc::LocalObjectAdapter<T> for RpcObjectAdapter");
            output("where");
            output("\tT: Interface,");
            output("{{");
            output("\tfn supports_interface(interface_id: canopy_rpc::InterfaceOrdinal) -> bool");
            output("\t{{");
            output("\t\tmatches_interface_id(interface_id)");
            output("\t}}");
            output("");
            output(
                "\tfn dispatch(implementation: &T, context: &canopy_rpc::DispatchContext, params: canopy_rpc::SendParams) -> canopy_rpc::SendResult");
            output("\t{{");
            output("\t\timplementation.__rpc_dispatch_generated(context, params)");
            output("\t}}");
            output("");
            output(
                "\tfn method_metadata(interface_id: canopy_rpc::InterfaceOrdinal) -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor]");
            output("\t{{");
            output("\t\tmethod_metadata_for_interface(interface_id)");
            output("\t}}");
            output("}}");
            output("");
            output("pub fn make_rpc_object<T>(implementation: T) -> std::sync::Arc<canopy_rpc::RpcBase<T, RpcObjectAdapter>>");
            output("where");
            output("\tT: Interface,");
            output("{{");
            output("\tcanopy_rpc::make_rpc_object_with_adapter::<T, RpcObjectAdapter>(implementation)");
            output("}}");
            output("");

            output("pub trait Proxy: Interface");
            output("{{");
            output("fn proxy_caller(&self) -> Option<&dyn canopy_rpc::GeneratedRpcCaller>");
            output("{{");
            output("\tNone");
            output("}}");
            output("");
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto method_module = sanitize_identifier(function->get_name());
                const auto analysed_params = analyse_rust_method_params(iface, *function, lib, root_module_name);
                const auto request_generics = generic_declaration_for_params(analysed_params, true, false);
                const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
                output(
                    "fn {}_request_shape() -> &'static str where Self: Sized",
                    method_module);
                output("{{");
                if (request_generics.empty())
                    output("\tstd::any::type_name::<interface_binding::{}::Request>()", method_module);
                else
                    output("\t\"interface_binding::{}::Request<...>\"", method_module);
                output("}}");
                output("");
                output(
                    "fn {}_response_shape() -> &'static str where Self: Sized",
                    method_module);
                output("{{");
                if (response_generics.empty())
                    output("\tstd::any::type_name::<interface_binding::{}::Response>()", method_module);
                else
                    output("\t\"interface_binding::{}::Response<...>\"", method_module);
                output("}}");
                output("");
            }
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                write_generated_delegate_method(output, iface, *function, "proxy_call", root_module_name, lib);
            }
            output("}}");
            output("");

            output("pub trait Stub: Interface");
            output("{{");
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto method_module = sanitize_identifier(function->get_name());
                const auto analysed_params = analyse_rust_method_params(iface, *function, lib, root_module_name);
                const auto all_generics = generic_declaration_for_params(analysed_params, true, true);
                const auto request_generics = generic_declaration_for_params(analysed_params, true, false);
                const auto response_generics = generic_declaration_for_params(analysed_params, false, true);
                auto all_where = generic_where_clause_for_params(analysed_params, true, true);
                if (all_where.empty())
                    all_where = " where Self: Sized";
                else
                    all_where += ", Self: Sized";

                output(
                    "fn {}_request_shape() -> &'static str where Self: Sized",
                    method_module);
                output("{{");
                if (request_generics.empty())
                    output("\tstd::any::type_name::<interface_binding::{}::Request>()", method_module);
                else
                    output("\t\"interface_binding::{}::Request<...>\"", method_module);
                output("}}");
                output("");
                output(
                    "fn {}_response_shape() -> &'static str where Self: Sized",
                    method_module);
                output("{{");
                if (response_generics.empty())
                    output("\tstd::any::type_name::<interface_binding::{}::Response>()", method_module);
                else
                    output("\t\"interface_binding::{}::Response<...>\"", method_module);
                output("}}");
                output("");
                output(
                    "fn stub_dispatch_local_{}{}(&self, service: &canopy_rpc::Service, caller_zone_id: canopy_rpc::CallerZone",
                    method_module,
                    all_generics);
                for (const auto& param : analysed_params)
                {
                    if (!param.is_in)
                        continue;

                    if (param.is_interface)
                        output.raw(", {}: &canopy_rpc::RemoteObject", param.rust_name);
                    else
                        output.raw(", {}: {}", param.rust_name, param.rust_type);
                }
                output.raw(") -> interface_binding::{}::LocalCallResult{}\n", method_module, all_generics);
                output("{}", all_where);
                output("{{");
                output(
                    "\tinterface_binding::{}::call_local::<Self{}>(service, caller_zone_id, self",
                    method_module,
                    all_generics.empty() ? "" : ", " + all_generics.substr(1, all_generics.size() - 2));
                for (const auto& param : analysed_params)
                {
                    if (!param.is_in)
                        continue;
                    output.raw(", {}", param.rust_name);
                }
                output.raw(")\n");
                output("}}");
                output("");
            }
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                write_generated_delegate_method(output, iface, *function, "stub_dispatch", root_module_name, lib);
            }
            output("}}");
            output("");

            output("pub struct ProxySkeleton;");
            output("impl canopy_rpc::CreateLocalProxy for ProxySkeleton {{}}");
            output("impl canopy_rpc::CastingInterface for ProxySkeleton");
            output("{{");
            output("\tfn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool");
            output("\t{{");
            output("\t\tmatches_interface_id(interface_id)");
            output("\t}}");
            output("}}");
            output("impl canopy_rpc::GeneratedRustInterface for ProxySkeleton");
            output("{{");
            output("\tfn interface_name() -> &'static str");
            output("\t{{");
            output("\t\tNAME");
            output("\t}}");
            output("");
            output("\tfn get_id(rpc_version: u64) -> u64");
            output("\t{{");
            output("\t\t<ProxySkeleton as Interface>::get_id(rpc_version)");
            output("\t}}");
            output("");
            output("\tfn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor]");
            output("\t{{");
            output("\t\tinterface_binding::METHODS");
            output("\t}}");
            output("}}");
            output("impl Interface for ProxySkeleton");
            output("{{");
            write_associated_type_impls(output, iface, lib, root_module_name);
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                write_generated_skeleton_method_body(output, iface, *function, "Proxy", "proxy_call", lib, root_module_name);
            }
            output("}}");
            output("impl Proxy for ProxySkeleton {{}}");
            output("");

            output("pub struct StubSkeleton;");
            output("impl canopy_rpc::CreateLocalProxy for StubSkeleton {{}}");
            output("impl canopy_rpc::CastingInterface for StubSkeleton");
            output("{{");
            output("\tfn __rpc_query_interface(&self, interface_id: canopy_rpc::InterfaceOrdinal) -> bool");
            output("\t{{");
            output("\t\tmatches_interface_id(interface_id)");
            output("\t}}");
            output("}}");
            output("impl canopy_rpc::GeneratedRustInterface for StubSkeleton");
            output("{{");
            output("\tfn interface_name() -> &'static str");
            output("\t{{");
            output("\t\tNAME");
            output("\t}}");
            output("");
            output("\tfn get_id(rpc_version: u64) -> u64");
            output("\t{{");
            output("\t\t<StubSkeleton as Interface>::get_id(rpc_version)");
            output("\t}}");
            output("");
            output("\tfn binding_metadata() -> &'static [canopy_rpc::GeneratedMethodBindingDescriptor]");
            output("\t{{");
            output("\t\tinterface_binding::METHODS");
            output("\t}}");
            output("}}");
            output("impl Interface for StubSkeleton");
            output("{{");
            write_associated_type_impls(output, iface, lib, root_module_name);
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;
                write_generated_skeleton_method_body(output, iface, *function, "Stub", "stub_dispatch", lib, root_module_name);
            }
            output("}}");
            output("impl Stub for StubSkeleton {{}}");
            output("");

            output("pub mod methods");
            output("{{");
            int method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
                {
                    output("pub const {}: u64 = {}u64;", uppercase_identifier(function->get_name()), method_count);
                    method_count++;
                }
            }
            output("}}");
            write_method_binding_metadata(output, iface, root_module_name, lib);
            output("}}");
        }

        void write_struct(
            writer& output,
            const class_entity& type_entity,
            const class_entity& lib,
            const std::string& root_module_name)
        {
            output("pub mod {}", sanitize_identifier(type_entity.get_name()));
            output("{{");
            output("pub const NAME: &str = \"{}\";", type_entity.get_name());

            for (const auto& version : protocol_versions)
            {
                output(
                    "pub const TYPE_ID_RPC_{}: u64 = {}u64;",
                    version.name,
                    fingerprint::generate(type_entity, {}, nullptr, version.value));
            }

            for (const auto& function : type_entity.get_functions())
            {
                if (function->get_entity_type() == entity_type::RUSTQUOTE)
                {
                    if (!function->is_in_import())
                        output.write_buffer(function->get_name());
                }
            }

            // Emit the Rust struct definition for non-template structs.
            if (can_emit_rust_value_struct(type_entity, lib))
            {
                output("#[derive(Debug, Clone, Default, PartialEq)]");
                output("pub struct Value {{");
                for (auto& member : type_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
                {
                    if (!member || member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                        continue;
                    auto func_entity = std::static_pointer_cast<function_entity>(member);
                    if (func_entity->is_static())
                        continue;
                    const auto field_name = sanitize_identifier(func_entity->get_name());
                    const auto field_type = rust_value_type_for_cpp_type_with_lib(
                        func_entity->get_return_type(), type_entity, lib, root_module_name);
                    output("\tpub {}: {},", field_name, field_type);
                }
                output("}}");
            }

            output("}}");
        }

        void write_namespace(
            writer& output,
            const class_entity& top_lib,
            const class_entity& scope,
            const std::string& root_module_name)
        {
            for (const auto& elem : scope.get_elements(entity_type::NAMESPACE_MEMBERS))
            {
                if (elem->is_in_import())
                    continue;

                if (elem->get_entity_type() == entity_type::NAMESPACE)
                {
                    output("pub mod {}", sanitize_identifier(elem->get_name()));
                    output("{{");
                    write_namespace(output, top_lib, static_cast<const class_entity&>(*elem), root_module_name);
                    output("}}");
                }
                else if (elem->get_entity_type() == entity_type::INTERFACE)
                {
                    write_interface(output, static_cast<const class_entity&>(*elem), root_module_name, top_lib);
                }
                else if (elem->get_entity_type() == entity_type::STRUCT)
                {
                    write_struct(output, static_cast<const class_entity&>(*elem), top_lib, root_module_name);
                }
                else if (elem->get_entity_type() == entity_type::RUSTQUOTE)
                {
                    output.write_buffer(elem->get_name());
                }
            }
        }
    } // namespace

    std::filesystem::path write_file(
        const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& relative_path)
    {
        auto rust_fs_path = output_path / "rust" / relative_path;
        rust_fs_path.replace_extension(".rs");
        std::filesystem::create_directories(rust_fs_path.parent_path());

        std::string existing_data;
        {
            std::ifstream existing_file(rust_fs_path);
            std::getline(existing_file, existing_data, '\0');
        }

        std::stringstream stream;
        writer output(stream);
        const auto root_module_name = relative_path.stem().string();
        output("// Generated by Canopy IDL generator. Do not edit.");
        output("// Rust output: interface IDs, method ordinals, binding metadata, and #rust_quote blocks.");
        output("");
        write_namespace(output, lib, lib, root_module_name);

        if (is_different(stream, existing_data))
        {
            std::ofstream file(rust_fs_path);
            file << stream.str();
        }

        return rust_fs_path;
    }
}
