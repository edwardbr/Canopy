/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <list>

#include "cpp_parser.h"
#include "coreclasses.h"
#include "writer.h"

[[nodiscard]] inline std::string_view string_piece(const std::string& value) noexcept
{
    return value;
}

[[nodiscard]] inline std::string_view string_piece(std::string_view value) noexcept
{
    return value;
}

[[nodiscard]] inline std::string_view string_piece(const char* value) noexcept
{
    return value == nullptr ? std::string_view{} : std::string_view{value};
}

template<typename... Parts> [[nodiscard]] std::string concat_strings(const Parts&... parts)
{
    std::string result;
    result.reserve((string_piece(parts).size() + ... + size_t{0}));
    (result.append(string_piece(parts)), ...);
    return result;
}

bool is_in_param(const attributes& attribs);
bool is_out_param(const attributes& attribs);
bool is_const_param(const attributes& attribs);

bool is_reference(std::string type_name);
bool is_rvalue(std::string type_name);
bool is_pointer(std::string type_name);
bool is_pointer_reference(std::string type_name);
bool is_pointer_to_pointer(std::string type_name);

std::string get_smart_ptr_type(
    const std::string& type_name,
    bool& is_optimistic);

bool is_interface_param(
    const class_entity& lib,
    const std::string& type,
    bool& is_optimistic,
    std::shared_ptr<class_entity>& obj);

std::string render_cpp_type(
    const class_entity& scope,
    const std::string& type);

bool is_type_and_parameter_the_same(
    std::string type,
    const std::string& name);

std::string cpp_string_literal(const std::string& value);

void render_parameter(
    writer& header,
    const class_entity& m_ob,
    const parameter_entity& parameter);

void render_function(
    writer& header,
    const class_entity& m_ob,
    const function_entity& function);
