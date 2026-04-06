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
#include <vector>

#include "coreclasses.h"
#include "helpers.h"
#include "proto_generator.h"
#include "writer.h"

namespace rust_protobuf_generator
{
    namespace
    {
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
                [&](const std::string& type_name) { return sanitize_identifier(type_name); });
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
            std::string generic_name;
            std::string trait_path;
            bool is_in = false;
            bool is_out = false;
        };

        std::vector<rust_interface_generic> analyse_interface_generics(
            const class_entity& iface,
            const function_entity& function)
        {
            std::vector<rust_interface_generic> result;
            size_t interface_index = 0;

            for (const auto& parameter : function.get_parameters())
            {
                bool is_optimistic = false;
                std::shared_ptr<class_entity> interface_entity;
                if (!is_interface_param(iface, parameter.get_type(), is_optimistic, interface_entity))
                    continue;

                rust_interface_generic generic;
                generic.generic_name = sanitize_identifier(parameter.get_name());
                std::transform(
                    generic.generic_name.begin(),
                    generic.generic_name.end(),
                    generic.generic_name.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                generic.generic_name += "Iface" + std::to_string(interface_index++);
                generic.trait_path = qualified_rust_module_path(*interface_entity) + "::Interface";
                generic.is_in = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                generic.is_out = is_out_param(parameter);
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

        std::string protobuf_namespace_path(const class_entity& entity)
        {
            std::vector<std::string> parts;

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
            const auto end_pos = proto_generator::extract_template_content(type_name, container_prefix.size() - 1, template_content);
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

        bool is_supported_protobuf_codegen_type(const std::string& cpp_type)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            if (is_supported_scalar_codegen_type(type_name))
                return true;

            std::string inner_type;
            return extract_vector_value_type(type_name, inner_type) && is_supported_scalar_codegen_type(inner_type);
        }

        bool supports_basic_protobuf_method_codegen(const function_entity& function)
        {
            for (const auto& parameter : function.get_parameters())
            {
                if (!is_supported_protobuf_codegen_type(parameter.get_type()))
                    return false;
            }

            const auto return_type = normalise_cpp_type(function.get_return_type());
            return return_type.empty() || return_type == "void" || is_supported_protobuf_codegen_type(return_type);
        }

        std::string protobuf_message_rust_type(
            const std::string& root_module_name,
            const std::string& message_name)
        {
            return "crate::__canopy_protobuf::" + sanitize_identifier(root_module_name) + "::" + message_name;
        }

        std::string protobuf_getter_expression(
            const std::string& message_name,
            const std::string& cpp_type,
            const std::string& field_name)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
            if (type_name == "std::vector<uint8_t>" || type_name == "std::vector<unsigned char>")
                return "canopy_rpc::serialization::protobuf::deserialize_bytes(" + message_name + "." + field_name + "())";
            if (type_name == "std::vector<char>" || type_name == "std::vector<signed char>")
                return "canopy_rpc::serialization::protobuf::deserialize_signed_bytes(" + message_name + "." + field_name + "())";
            std::string vector_value_type;
            if (extract_vector_value_type(type_name, vector_value_type))
            {
                if (vector_value_type == "std::string")
                    return message_name + "." + field_name + "().into_iter().map(|value| value.to_string()).collect()";
                if (vector_value_type == "size_t")
                    return message_name + "." + field_name
                         + "().into_iter().map(|value| usize::try_from(value).map_err(|_| canopy_rpc::INVALID_DATA())).collect::<Result<Vec<_>, _>>()?";
                return message_name + "." + field_name + "().into_iter().collect()";
            }
            if (type_name == "std::string")
                return message_name + "." + field_name + "().to_string()";
            return message_name + "." + field_name + "()";
        }

        std::string protobuf_setter_argument_expression(
            const std::string& value_name,
            const std::string& cpp_type)
        {
            const auto type_name = normalise_cpp_type(cpp_type);
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
                return value_name + ".iter().copied()";
            }
            if (type_name == "std::string")
                return value_name + ".as_str()";
            if (type_name == "size_t")
                return "u64::try_from(" + value_name + ").map_err(|_| canopy_rpc::INVALID_DATA())?";
            return value_name;
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

            auto add_target =
                [&](const build_target_descriptor& target)
                {
                    if (target.manifest_path == self_target.manifest_path)
                        return;
                    if (!seen_manifests.insert(target.manifest_path).second)
                        return;
                    result.push_back(target);
                };

            add_target({
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
                add_target({
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
                    "\tProtobufBuildTarget {{ crate_name: \"{}\", output_subdir: \"{}\", manifest_path: \"{}\", master_proto: \"{}\" }},",
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

            output("pub mod {}", sanitize_identifier(iface.get_name()));
            output("{{");
            output("pub mod interface_binding");
            output("{{");
            output("pub struct BindingMetadata;");
            output("");

            int method_count = 1;
            for (const auto& function : iface.get_functions())
            {
                if (function->get_entity_type() != entity_type::FUNCTION_METHOD)
                    continue;

                const auto module_name = sanitize_identifier(function->get_name());
                const auto request_message = iface.get_name() + "_" + function->get_name() + "Request";
                const auto response_message = iface.get_name() + "_" + function->get_name() + "Response";
                const auto interface_generics = analyse_interface_generics(iface, *function);
                const auto request_generics = generic_declaration_for_params(interface_generics, true, false);
                const auto response_generics = generic_declaration_for_params(interface_generics, false, true);
                const auto codec_generics = generic_declaration_for_params(interface_generics, true, true);
                const auto codec_where = generic_where_clause_for_params(interface_generics, true, true);
                const auto interface_module_path = "crate::" + root_module_name + "::" + qualified_rust_module_path(iface)
                                                 + "::interface_binding::" + module_name;
                const auto interface_trait_path = "crate::" + root_module_name + "::" + qualified_rust_module_path(iface)
                                                + "::Interface";
                const auto proto_request_type = protobuf_message_rust_type(root_module_name, request_message);
                const auto proto_response_type = protobuf_message_rust_type(root_module_name, response_message);
                const bool supports_codegen = interface_generics.empty() && supports_basic_protobuf_method_codegen(*function);

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
                    const bool param_is_in = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                    output(
                        "\tcanopy_rpc::serialization::protobuf::GeneratedProtobufParamDescriptor {{ name: \"{}\", field_number: {}u32, direction: "
                        "canopy_rpc::ParameterDirection::{}, proto_type: \"{}\", field_kind: "
                        "canopy_rpc::serialization::protobuf::GeneratedProtobufFieldKind::{} }},",
                        parameter.get_name(),
                        protobuf_field_number,
                        direction_name(param_is_in, is_out_param(parameter)),
                        cpp_type_to_proto_type(iface, lib, parameter.get_type()),
                        protobuf_field_kind(iface, lib, parameter.get_type()));
                    ++protobuf_field_number;
                }
                output("];");
                output("");
                output("pub struct ProtobufCodec{}(std::marker::PhantomData<fn() -> ()>);", codec_generics);
                output("");
                output(
                    "impl{} canopy_rpc::serialization::protobuf::GeneratedProtobufMethodCodec for ProtobufCodec{}{}",
                    codec_generics,
                    codec_generics,
                    codec_where);
                output("{{");
                output("\ttype Request = {}::DispatchRequest{};", interface_module_path, request_generics);
                output("\ttype Response = {}::DispatchResponse{};", interface_module_path, response_generics);
                if (supports_codegen)
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
                output(
                    "\tfn descriptor() -> &'static canopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor");
                output("\t{{");
                output("\t\tsuper::protobuf_by_method_id({}u64).expect(\"protobuf method descriptor\")", method_count);
                output("\t}}");
                output("");
                if (supports_codegen)
                {
                    output("\tfn request_from_protobuf_message(message: Self::ProtoRequest) -> Result<Self::Request, i32>");
                    output("\t{{");
                    output("\t\tOk(Self::Request {{");
                    for (const auto& parameter : function->get_parameters())
                    {
                        const bool param_is_in =
                            is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                        if (!param_is_in)
                            continue;
                        output(
                            "\t\t\t{}: {},",
                            sanitize_identifier(parameter.get_name()),
                            protobuf_getter_expression("message", parameter.get_type(), sanitize_identifier(parameter.get_name())));
                    }
                    output("\t\t}})");
                    output("\t}}");
                    output("");
                    output("\tfn response_to_protobuf_message(response: &Self::Response) -> Result<Self::ProtoResponse, i32>");
                    output("\t{{");
                    output("\t\tlet mut message = Self::ProtoResponse::new();");
                    for (const auto& parameter : function->get_parameters())
                    {
                        if (!is_out_param(parameter))
                            continue;
                        const auto field_name = sanitize_identifier(parameter.get_name());
                        output(
                            "\t\tmessage.set_{}({});",
                            field_name,
                            protobuf_setter_argument_expression("response." + field_name, parameter.get_type()));
                    }
                    if (!function->get_return_type().empty() && normalise_cpp_type(function->get_return_type()) != "void")
                        output(
                            "\t\tmessage.set_result({});",
                            protobuf_setter_argument_expression("response.return_value", function->get_return_type()));
                    output("\t\tOk(message)");
                    output("\t}}");
                }
                else
                {
                    output("\tfn request_from_protobuf_message(_message: Self::ProtoRequest) -> Result<Self::Request, i32>");
                    output("\t{{");
                    output("\t\tErr(canopy_rpc::INVALID_DATA())");
                    output("\t}}");
                    output("");
                    output("\tfn response_to_protobuf_message(_response: &Self::Response) -> Result<Self::ProtoResponse, i32>");
                    output("\t{{");
                    output("\t\tErr(canopy_rpc::INVALID_DATA())");
                    output("\t}}");
                }
                output("}}");
                output("");
                output("fn request_to_protobuf_message{}(request: &{}::Request{}) -> Result<{}, i32>{}",
                    codec_generics,
                    interface_module_path,
                    request_generics,
                    supports_codegen ? proto_request_type : "canopy_rpc::serialization::protobuf::UnsupportedGeneratedMessage",
                    codec_where);
                output("{{");
                if (supports_codegen)
                {
                    output("\tlet mut message = {}::new();", proto_request_type);
                    for (const auto& parameter : function->get_parameters())
                    {
                        const bool param_is_in =
                            is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                        if (!param_is_in)
                            continue;
                        const auto field_name = sanitize_identifier(parameter.get_name());
                        output(
                            "\tmessage.set_{}({});",
                            field_name,
                            protobuf_setter_argument_expression("request." + field_name, parameter.get_type()));
                    }
                    output("\tOk(message)");
                }
                else
                {
                    output("\tlet _ = request;");
                    output("\tErr(canopy_rpc::INVALID_DATA())");
                }
                output("}}");
                output("");
                output("fn response_from_protobuf_message{}(message: {}) -> Result<{}::Response{}, i32>{}",
                    codec_generics,
                    supports_codegen ? proto_response_type : "canopy_rpc::serialization::protobuf::UnsupportedGeneratedMessage",
                    interface_module_path,
                    response_generics,
                    codec_where);
                output("{{");
                if (supports_codegen)
                {
                    output("\tOk({}::Response {{", interface_module_path);
                    for (const auto& parameter : function->get_parameters())
                    {
                        if (!is_out_param(parameter))
                            continue;
                        output(
                            "\t\t{}: {},",
                            sanitize_identifier(parameter.get_name()),
                            protobuf_getter_expression("message", parameter.get_type(), sanitize_identifier(parameter.get_name())));
                    }
                    if (!function->get_return_type().empty() && normalise_cpp_type(function->get_return_type()) != "void")
                        output(
                            "\t\treturn_value: {},",
                            protobuf_getter_expression("message", function->get_return_type(), "result"));
                    output("\t}})");
                }
                else
                {
                    output("\tlet _ = message;");
                    output("\tErr(canopy_rpc::INVALID_DATA())");
                }
                output("}}");
                output("");
                output("pub fn encode_request{}(request: &{}::Request{}) -> Result<Vec<u8>, i32>{}",
                    codec_generics,
                    interface_module_path,
                    request_generics,
                    codec_where);
                output("{{");
                output("\tlet message = request_to_protobuf_message{}(request)?;", codec_generics);
                output("\tcanopy_rpc::serialization::protobuf::serialize_generated_message(&message)");
                output("}}");
                output("");
                output("pub fn decode_response{}(proto_bytes: &[u8]) -> Result<{}::Response{}, i32>{}",
                    codec_generics,
                    interface_module_path,
                    response_generics,
                    codec_where);
                output("{{");
                output(
                    "\tlet message: {} = canopy_rpc::serialization::protobuf::parse_generated_message(proto_bytes)?;",
                    supports_codegen ? proto_response_type : "canopy_rpc::serialization::protobuf::UnsupportedGeneratedMessage");
                output("\tresponse_from_protobuf_message{}(message)", codec_generics);
                output("}}");
                output("");
                output("#[doc(hidden)]");
                output(
                    "pub fn dispatch_generated<Impl{}>(implementation: &Impl, context: &canopy_rpc::DispatchContext, params: canopy_rpc::SendParams) -> canopy_rpc::SendResult",
                    codec_generics.empty() ? "" : ", " + codec_generics.substr(1, codec_generics.size() - 2));
                if (codec_where.empty())
                    output(" where Impl: {}", interface_trait_path);
                else
                    output("{}, Impl: {}", codec_where, interface_trait_path);
                output("{{");
                output(
                    "\tcanopy_rpc::serialization::protobuf::dispatch_generated_stub_call::<ProtobufCodec{}, _>(params, |request|",
                    codec_generics);
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
                    "\tcanopy_rpc::serialization::protobuf::GeneratedProtobufMethodDescriptor {{ method_name: \"{}\", method_id: {}u64, "
                    "schema_proto_file: {}::PROTOBUF_SCHEMA_FILE, proto_package: {}::PROTOBUF_PACKAGE, "
                    "request_message: {}::PROTOBUF_REQUEST_MESSAGE, response_message: {}::PROTOBUF_RESPONSE_MESSAGE, "
                    "request_proto_full_name: {}::PROTOBUF_REQUEST_FULL_NAME, response_proto_full_name: {}::PROTOBUF_RESPONSE_FULL_NAME, "
                    "request_rust_type_name: {}::PROTOBUF_REQUEST_RUST_TYPE, response_rust_type_name: {}::PROTOBUF_RESPONSE_RUST_TYPE, "
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
                    write_interface(output, lib, static_cast<const class_entity&>(*elem), root_module_name);
                }
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
        auto rust_fs_path = output_path / "rust" / relative_path.parent_path()
                          / (relative_path.stem().string() + "_protobuf.rs");
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
