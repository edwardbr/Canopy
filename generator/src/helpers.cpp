/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <iostream>

#include <fmt/format.h>

#include "helpers.h"
#include "coreclasses.h"
#include "attributes.h"

std::string get_smart_ptr_type(const std::string& type_name, bool& is_optimistic)
{
    auto data = type_name.data();
    while (*data == ' ' || *data == '\t')
    {
        data++;
    }

    bool found = false;
    if (begins_with(data, "rpc::shared_ptr<"))
    {
        data += sizeof("rpc::shared_ptr");
        found = true;
    }
    else if (begins_with(data, "rpc::optimistic_ptr<"))
    {
        found = true;
        data += sizeof("rpc::optimistic_ptr");
        is_optimistic = true;
    }

    if (found)
    {
        // auto pos = data - type_name.data();
        auto rpos = type_name.rfind(">");
        if (rpos == std::string::npos)
        {
            std::cerr << fmt::format("template parameter is malformed {}", type_name);
            throw fmt::format("template parameter is malformed {}", type_name);
        }
        if (rpos != type_name.size() - 1)
        {
            std::cerr << fmt::format("template parameter is malformed {}", type_name);
            throw fmt::format("template parameter is malformed {}", type_name);
        }
        std::string_view interface_name(data, &type_name[rpos]);
        // auto interface_name = type_name.substr(pos, rpos - pos);
        if (interface_name.find('<') != std::string::npos && interface_name.find('>') != std::string::npos
            && interface_name.find(' ') != std::string::npos)
        {
            std::cerr << fmt::format("nested templates are not supported {}", type_name);
            throw fmt::format("nested templates are not supported {}", type_name);
        }
        return std::string(interface_name);
    }
    return "";
}

bool is_interface_param(
    const class_entity& lib, const std::string& type, bool& is_optimistic, std::shared_ptr<class_entity>& obj)
{
    std::string reference_modifiers;
    std::string type_name = type;
    strip_reference_modifiers(type_name, reference_modifiers);

    std::string encapsulated_type = get_smart_ptr_type(type_name, is_optimistic);
    if (encapsulated_type.empty())
        return false;

    if (lib.find_class(encapsulated_type, obj))
    {
        if (obj->get_entity_type() == entity_type::INTERFACE)
        {
            return true;
        }
    }
    return false;
}

bool is_in_param(const attributes& attribs)
{
    return attribs.has_value(attribute_types::in_param);
}

bool is_out_param(const attributes& attribs)
{
    return attribs.has_value(attribute_types::out_param);
}

bool is_const_param(const attributes& attribs)
{
    return attribs.has_value(attribute_types::const_function);
}

bool is_reference(std::string type_name)
{
    std::string reference_modifiers;
    strip_reference_modifiers(type_name, reference_modifiers);

    return reference_modifiers == "&";
}

bool is_rvalue(std::string type_name)
{
    std::string reference_modifiers;
    strip_reference_modifiers(type_name, reference_modifiers);

    return reference_modifiers == "&&";
}

bool is_pointer(std::string type_name)
{
    std::string reference_modifiers;
    strip_reference_modifiers(type_name, reference_modifiers);

    return reference_modifiers == "*";
}

bool is_pointer_reference(std::string type_name)
{
    std::string reference_modifiers;
    strip_reference_modifiers(type_name, reference_modifiers);

    return reference_modifiers == "*&";
}

bool is_pointer_to_pointer(std::string type_name)
{
    std::string reference_modifiers;
    strip_reference_modifiers(type_name, reference_modifiers);

    return reference_modifiers == "**";
}

bool is_type_and_parameter_the_same(std::string type, std::string name)
{
    if (type.empty() || type.size() < name.size())
        return false;
    if (*type.rbegin() == '&' || *type.rbegin() == '*')
    {
        type = type.substr(0, type.size() - 1);
    }
    auto template_pos = type.find('<');
    if (template_pos == 0)
    {
        return false;
    }

    if (template_pos != std::string::npos)
    {
        type = type.substr(0, template_pos);
    }
    return type == name;
}

void render_parameter(writer& wrtr, const class_entity& m_ob, const parameter_entity& parameter)
{
    std::string modifier;
    bool has_struct = false;
    if (parameter.has_value("const"))
        modifier = "const " + modifier;
    if (parameter.has_value("struct"))
        has_struct = true;

    if (has_struct)
    {
        modifier = modifier + "struct ";
    }
    else if (is_type_and_parameter_the_same(parameter.get_type(), parameter.get_name()))
    {
        std::shared_ptr<class_entity> obj;
        if (!m_ob.get_owner()->find_class(parameter.get_name(), obj))
        {
            throw std::runtime_error(std::string("unable to identify type ") + parameter.get_name());
        }
        auto type = obj->get_entity_type();
        if (type == entity_type::STRUCT)
            modifier = modifier + "struct ";
        else if (type == entity_type::ENUM)
            modifier = modifier + "enum ";
    }

    wrtr.raw("{}{} {}", modifier, parameter.get_type(), parameter.get_name());
}

void render_function(writer& wrtr, const class_entity& m_ob, const function_entity& function)
{
    std::string modifier;
    if (function.is_static())
    {
        modifier += "inline static ";
    }
    bool has_struct = function.has_value("struct");
    if (has_struct)
    {
        modifier = modifier + "struct ";
    }
    else if (is_type_and_parameter_the_same(function.get_return_type(), function.get_name()))
    {
        std::shared_ptr<class_entity> obj;
        if (!m_ob.get_owner()->find_class(function.get_name(), obj))
        {
            throw std::runtime_error(std::string("unable to identify type ") + function.get_name());
        }
        auto type = obj->get_entity_type();
        if (type == entity_type::STRUCT)
            modifier = modifier + "struct ";
        else if (type == entity_type::ENUM)
            modifier = modifier + "enum ";
    }

    wrtr.raw("{}{} {}", modifier, function.get_return_type(), function.get_name());
}
