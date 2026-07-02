/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// NOLINTNEXTLINE(bugprone-incorrect-enable-shared-from-this): class_entity is defined in the idlparser submodule.
class class_entity;

namespace proto_generator
{
    std::string trim_copy(std::string value);

    std::string normalise_cpp_type(std::string type_name);

    size_t extract_template_content(
        const std::string& type,
        size_t start_pos,
        std::string& content);

    bool split_template_args(
        const std::string& args,
        std::string& first,
        std::string& second);

    std::vector<std::string> split_template_args(const std::string& args);

    bool is_map_type(
        const std::string& type,
        std::string& prefix);

    bool is_sequence_type(
        const std::string& type,
        std::string& prefix);

    bool is_optional_type(
        const std::string& type,
        std::string& inner_type);

    std::string optional_inner_type(const std::string& type);

    bool is_nullable_optional_type(
        const std::string& type,
        std::string& inner_type);

    std::string nullable_optional_inner_type(const std::string& type);

    bool is_variant_type(
        const std::string& type,
        std::vector<std::string>& alternative_types);

    std::string cpp_scalar_to_proto_type(const std::string& type);

    std::string sanitize_type_name(const std::string& type_name);

    std::string sanitize_field_name(const std::string& field_name);

    bool is_enum_type(
        const class_entity& lib,
        const std::string& type_name);

    bool is_json_dom_type(const std::string& type_name);

    std::string cpp_type_to_proto_type(const std::string& cpp_type);

    std::string cpp_type_to_proto_type(
        const std::string& cpp_type,
        const std::function<bool(const std::string&)>& is_interface_type,
        const std::function<bool(const std::string&)>& is_enum_type,
        const std::function<std::string(const std::string&)>& sanitize_custom_type);
}
