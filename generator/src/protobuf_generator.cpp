/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <cctype>
#include <map>
#include <functional>
#include <set>

// Other headers
extern "C"
{
#include "sha3.h"
}
#include "attributes.h"
#include "coreclasses.h"
#include "cpp_parser.h"
#include "fingerprint_generator.h"
#include "helpers.h"
#include "interface_declaration_generator.h"
#include "type_utils.h"
#include "writer.h"
#include "protobuf_generator.h"

namespace protobuf_generator
{
    // Track concrete template instantiations that need to be generated
    struct TemplateInstantiation
    {
        std::string template_name;  // e.g., "test_template"
        std::string template_param; // e.g., "int"
        std::string concrete_name;  // e.g., "test_template_int"
    };

    // Forward declarations
    std::string sanitize_type_name(const std::string& type_name);
    void write_single_interface_service(const class_entity& lib,
        const std::shared_ptr<class_entity>& interface_entity,
        writer& proto,
        std::set<std::shared_ptr<class_entity>>& referenced_interfaces,
        std::set<std::shared_ptr<class_entity>>& referenced_packages);

    // Helper function to get the full namespace prefix for proto filenames
    // Collects all namespace names from root to current entity
    // e.g., for v1::foo in websocket_demo, returns "websocket_demo_v1"
    std::string get_namespace_name(const class_entity& current_lib)
    {
        get_full_name(current_lib, true, true, "_");
        std::string prefix = "";
        if (current_lib.get_owner())
        {
            prefix = get_namespace_name(*current_lib.get_owner());
        }

        if (!prefix.empty())
        {
            prefix += "_";
        }
        prefix += current_lib.get_name();

        return prefix;
    }

    // Helper function to get full namespace prefix for an interface entity
    // Walks up the owner chain to build the full namespace path
    std::string get_interface_namespace_prefix(
        const std::string& module_name, const class_entity& lib, const class_entity& interface_entity)
    {
        std::vector<std::string> namespace_parts;

        // Walk up the owner chain to collect namespace names
        const class_entity* current = interface_entity.get_owner();
        while (current != nullptr && !current->get_name().empty())
        {
            // Don't include the root lib entity (which has no name)
            if (current != &lib)
            {
                namespace_parts.push_back(current->get_name());
            }
            current = current->get_owner();
        }

        // Build the prefix starting with the module name
        std::string prefix = "";
        if (!module_name.empty())
        {
            prefix = module_name;
        }

        // Add namespace parts in reverse order (from outer to inner)
        // Skip parts that duplicate the module name
        // NOLINTNEXTLINE(modernize-loop-convert): reverse_view is not available in this toolchain configuration.
        for (auto it = namespace_parts.rbegin(); it != namespace_parts.rend(); ++it)
        {
            // Don't duplicate the module name
            if (*it == module_name && prefix == module_name)
            {
                continue;
            }

            if (!prefix.empty())
            {
                prefix += "_";
            }
            prefix += *it;
        }

        // If prefix is still empty (shouldn't happen), use "default"
        if (prefix.empty())
        {
            prefix = "default";
        }

        return prefix;
    }

    // Helper function to trim whitespace from both ends of a string
    std::string trim_whitespace(const std::string& str)
    {
        size_t start = str.find_first_not_of(' ');
        if (start == std::string::npos)
            return "";
        size_t end = str.find_last_not_of(' ');
        return str.substr(start, end - start + 1);
    }

    // Helper function to extract template arguments, properly handling nested templates
    // Returns the position after the closing '>' bracket
    size_t extract_template_content(const std::string& type, size_t start_pos, std::string& content)
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

    // Helper function to split template arguments at the top-level comma
    // Handles nested templates like std::map<std::string, std::vector<int>>
    bool split_template_args(const std::string& args, std::string& first, std::string& second)
    {
        int bracket_count = 0;
        for (size_t i = 0; i < args.length(); ++i)
        {
            char c = args[i];
            if (c == '<')
                bracket_count++;
            else if (c == '>')
                bracket_count--;
            else if (c == ',' && bracket_count == 0)
            {
                first = trim_whitespace(args.substr(0, i));
                second = trim_whitespace(args.substr(i + 1));
                return true;
            }
        }
        return false;
    }

    // Helper function to check if a type is a map container type
    // Supports std::map, std::unordered_map, and std::flat_map
    bool is_map_type(const std::string& type, std::string& prefix)
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

    // Helper function to check if a type is a vector/array container type
    bool is_sequence_type(const std::string& type, std::string& prefix)
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

    // Helper function to convert scalar C++ types to protobuf scalar types
    // Returns empty string if not a recognized scalar type
    std::string cpp_scalar_to_proto_type(const std::string& type)
    {
        // Common typedefs
        if (type == "error_code")
            return "int32";

        // Signed integer types
        // int8_t, signed char -> int32 (protobuf has no int8)
        if (type == "int8_t" || type == "signed char")
            return "int32";

        // int16_t, short -> int32 (protobuf has no int16)
        if (type == "int16_t" || type == "short" || type == "short int" || type == "signed short"
            || type == "signed short int")
            return "int32";

        // int32_t, int
        if (type == "int32_t" || type == "int" || type == "signed int")
            return "int32";

        // int64_t, long long, long (long is 64-bit on most platforms, safer to use int64)
        if (type == "int64_t" || type == "long" || type == "long int" || type == "signed long"
            || type == "signed long int" || type == "long long" || type == "signed long long" || type == "long long int"
            || type == "signed long long int")
            return "int64";

        // Unsigned integer types
        // uint8_t, unsigned char -> uint32 (protobuf has no uint8)
        if (type == "uint8_t" || type == "unsigned char")
            return "uint32";

        // uint16_t, unsigned short -> uint32 (protobuf has no uint16)
        if (type == "uint16_t" || type == "unsigned short" || type == "unsigned short int")
            return "uint32";

        // uint32_t, unsigned int, unsigned
        if (type == "uint32_t" || type == "unsigned int")
            return "uint32";

        // uint64_t, unsigned long long, unsigned long
        if (type == "uint64_t" || type == "unsigned long" || type == "unsigned long int" || type == "unsigned long long"
            || type == "unsigned long long int")
            return "uint64";

        // Platform-specific types
        if (type == "size_t")
            return "uint64";
        if (type == "ptrdiff_t" || type == "ssize_t" || type == "intptr_t")
            return "int64";
        if (type == "uintptr_t")
            return "uint64";

        // Floating point types
        if (type == "float")
            return "float";
        if (type == "double" || type == "long double")
            return "double";

        // Boolean
        if (type == "bool")
            return "bool";

        // Character types (mapped to int32 since protobuf has no char type)
        if (type == "char" || type == "wchar_t" || type == "char16_t" || type == "char32_t")
            return "int32";

        // String types
        if (type == "std::string" || type == "string")
            return "string";

        // C-style strings
        if (type == "char*" || type == "const char*" || type == "char *" || type == "const char *")
            return "string";

        // Not a recognized scalar type
        return "";
    }

    // Helper function to convert C++ type to Protocol Buffers type
    std::string cpp_type_to_proto_type(const std::string& cpp_type)
    {
        std::string type = cpp_type;

        // Remove const qualifier
        if (type.find("const ") == 0)
            type = type.substr(6);

        // Check if this is a pointer type BEFORE removing the pointer
        // Pointers marshal the address only (uint64), not the data
        bool is_pointer = (type.find('*') != std::string::npos);

        // Remove reference and pointer modifiers
        size_t pos = type.find('&');
        if (pos != std::string::npos)
            type = type.substr(0, pos);

        pos = type.find('*');
        if (pos != std::string::npos)
            type = type.substr(0, pos);

        // Trim whitespace
        type = trim_whitespace(type);

        // If this was a pointer, marshal as address only
        if (is_pointer)
            return "uint64";

        // Special handling for byte vectors -> protobuf bytes
        // std::vector<uint8_t>, std::vector<char> are binary data, not integer arrays
        if (type == "std::vector<uint8_t>" || type == "std::vector<unsigned char>" || type == "std::vector<char>"
            || type == "std::vector<signed char>")
            return "bytes";

        // Check for map types (std::map, std::unordered_map, std::flat_map)
        std::string container_prefix;
        if (is_map_type(type, container_prefix))
        {
            size_t template_start = type.find('<');
            std::string inner_content;
            if (extract_template_content(type, template_start, inner_content) != std::string::npos)
            {
                std::string key_type;
                std::string value_type;
                if (split_template_args(inner_content, key_type, value_type))
                {
                    std::string proto_key_type = cpp_type_to_proto_type(key_type);
                    std::string proto_value_type = cpp_type_to_proto_type(value_type);
                    return "map<" + proto_key_type + ", " + proto_value_type + ">";
                }
            }
            // Fallback for malformed map
            return "map<string, string>";
        }

        // Check for sequence types (std::vector, std::array)
        if (is_sequence_type(type, container_prefix))
        {
            size_t template_start = type.find('<');
            std::string inner_content;
            if (extract_template_content(type, template_start, inner_content) != std::string::npos)
            {
                if (container_prefix == "std::array<")
                {
                    // For std::array<T, N>, extract just the T part (first template parameter)
                    std::string element_type;
                    std::string size_param;
                    if (split_template_args(inner_content, element_type, size_param))
                    {
                        std::string inner_proto_type = cpp_type_to_proto_type(element_type);
                        return "repeated " + inner_proto_type;
                    }
                }
                // For std::vector<T>, the inner_content is just T
                std::string inner_proto_type = cpp_type_to_proto_type(inner_content);
                return "repeated " + inner_proto_type;
            }
            // Fallback
            return "repeated string";
        }

        // Handle rpc::shared_ptr<T> (interface types)
        if (type.find("rpc::shared_ptr<") == 0 || type.find("rpc::optimistic_ptr<") == 0)
        {
            // Use the unified interface_descriptor for all interface pointer types
            return "rpc.interface_descriptor";
        }

        // Handle 128-bit integers using the dedicated uint128 message type (two uint64 halves).
        // Protobuf has no native 128-bit type; uint128 {lo, hi} is emitted into the proto file
        // whenever a struct field of this type is encountered.
        if (type == "unsigned __int128" || type == "__int128" || type == "uint128_t" || type == "int128_t")
            return "uint128";

        // Check scalar types
        std::string scalar_type = cpp_scalar_to_proto_type(type);
        if (!scalar_type.empty())
            return scalar_type;

        // Check if it's a user-defined template instantiation (e.g., test_template<int>)
        size_t template_start = type.find('<');
        if (template_start != std::string::npos && type.find('>') != std::string::npos)
        {
            // It's a template instantiation
            std::string template_name = type.substr(0, template_start);
            std::string inner_content;

            if (extract_template_content(type, template_start, inner_content) != std::string::npos)
            {
                // Convert template parameters to a sanitized suffix
                std::string sanitized_suffix = inner_content;

                // Replace common types with short names for readability
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
                {
                    // For complex types, sanitize the parameter string
                    sanitized_suffix = sanitize_type_name(inner_content);
                }

                // Return the concrete type name
                return template_name + "_" + sanitized_suffix;
            }
        }

        // For custom types (structs, classes, interfaces), return the type name as is
        // This will be handled by the message definition
        return type;
    }

    // Helper function to sanitize type name for protobuf
    std::string sanitize_type_name(const std::string& type_name)
    {
        std::string result = type_name;

        // Convert C++ namespace separators (::) to protobuf package separators (.)
        // This allows cross-package references like rpc::encoding -> rpc.encoding
        size_t pos = 0;
        while ((pos = result.find("::", pos)) != std::string::npos)
        {
            result.replace(pos, 2, ".");
            pos += 1;
        }

        // Ensure the name starts with a letter
        if (!result.empty() && !std::isalpha(result[0]) && result[0] != '_')
            result = "_" + result;

        // Replace invalid characters with underscore (but preserve dots for package names)
        for (auto& c : result)
        {
            if (!std::isalnum(c) && c != '_' && c != '.')
                c = '_';
        }

        return result;
    }

    // Helper function to sanitize field name for protobuf
    std::string sanitize_field_name(const std::string& field_name)
    {
        std::string result = field_name;

        // Replace invalid characters with underscore
        for (auto& c : result)
        {
            if (!std::isalnum(c) && c != '_')
                c = '_';
        }

        // Ensure the name starts with a letter or underscore
        if (!result.empty() && !std::isalpha(result[0]) && result[0] != '_')
            result = "_" + result;

        return result;
    }

    // Helper function to get the fully scoped name for a type
    void build_fully_scoped_name(const class_entity* entity, std::string& name)
    {
        auto* owner = entity->get_owner();
        if (owner && !owner->get_name().empty())
        {
            build_fully_scoped_name(owner, name);
            name += ".";
        }
        name += sanitize_type_name(entity->get_name());
    }

    // Helper function to write a message definition for a struct/class
    void write_message_definition(const class_entity& entity, writer& proto, int& field_number)
    {
        // Start the message definition
        proto("message {} {{", sanitize_type_name(entity.get_name()));

        // Process fields of the struct/class - these would be function entities in the STRUCTURE_MEMBERS
        // In IDL, struct members are typically represented as function entities with variable type
        for (auto& member : entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            // For structs, we need to look for function_variable entities which represent struct fields
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members - they are class-level, not instance-level
                if (func_entity->is_static())
                    continue;

                // For struct fields, the return type is the field type and the name is the field name
                std::string field_type = cpp_type_to_proto_type(func_entity->get_return_type());
                std::string field_name = sanitize_field_name(func_entity->get_name());

                // For custom types with namespace, sanitize only the type name part
                // Handle "repeated TypeName" and "map<K, V>" specially to preserve keywords
                if (field_type.find("::") != std::string::npos)
                {
                    if (field_type.find("repeated ") == 0)
                    {
                        // Extract and sanitize just the type name after "repeated "
                        std::string type_name = field_type.substr(9); // Skip "repeated "
                        field_type = "repeated " + sanitize_type_name(type_name);
                    }
                    else if (field_type.find("map<") == 0)
                    {
                        // Map types are already in correct format, don't sanitize
                        // Just remove C++ namespace separators in the inner types
                        size_t pos = 0;
                        while ((pos = field_type.find("::", pos)) != std::string::npos)
                        {
                            field_type.replace(pos, 2, ".");
                            pos += 1;
                        }
                    }
                    else
                    {
                        // Simple custom type, sanitize normally
                        field_type = sanitize_type_name(field_type);
                    }
                }

                field_number++;
                proto("{} {} = {};", field_type, field_name, field_number);
            }
            // Also handle typedefs that might be part of the struct
            else if (member->get_entity_type() == entity_type::TYPEDEF)
            {
                // Skip typedefs for now, they are handled separately
            }
        }

        // Close the message definition
        proto("}}");
        proto("");
    }

    // Helper function to write a concrete template instantiation message
    void write_template_instantiation(const class_entity& template_entity,
        const std::string& template_param,
        const std::string& concrete_name,
        writer& proto,
        int& field_number)
    {
        // Start the message definition with the concrete name
        proto("message {} {{", concrete_name);

        // Process fields of the template struct, substituting template parameters
        for (auto& member : template_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);
                std::string field_type = func_entity->get_return_type();
                std::string field_name = sanitize_field_name(func_entity->get_name());

                // Substitute template parameter T with the concrete type
                if (field_type == "T")
                {
                    field_type = template_param;
                }

                // Convert to proto type
                field_type = cpp_type_to_proto_type(field_type);

                // For custom types with namespace, sanitize only the type name part
                // Handle "repeated TypeName" and "map<K, V>" specially to preserve keywords
                if (field_type.find("::") != std::string::npos)
                {
                    if (field_type.find("repeated ") == 0)
                    {
                        // Extract and sanitize just the type name after "repeated "
                        std::string type_name = field_type.substr(9); // Skip "repeated "
                        field_type = "repeated " + sanitize_type_name(type_name);
                    }
                    else if (field_type.find("map<") == 0)
                    {
                        // Map types are already in correct format, don't sanitize
                        // Just remove C++ namespace separators in the inner types
                        size_t pos = 0;
                        while ((pos = field_type.find("::", pos)) != std::string::npos)
                        {
                            field_type.replace(pos, 2, ".");
                            pos += 1;
                        }
                    }
                    else
                    {
                        // Simple custom type, sanitize normally
                        field_type = sanitize_type_name(field_type);
                    }
                }

                field_number++;
                proto("{} {} = {};", field_type, field_name, field_number);
            }
        }

        // Close the message definition
        proto("}}");
        proto("");
    }

    // NOTE: Previously generated per-interface _ptr structures, but now we use
    // the unified rpc.interface_descriptor type instead

    // Helper function to process imports from IDL imports
    void write_imports(const class_entity& lib, writer& proto)
    {
        // Process imported entities to generate import statements
        // We'll collect unique import libraries from the classes in the entity
        std::set<std::string> unique_imports;

        for (auto& cls : lib.get_classes())
        {
            if (!cls->get_import_lib().empty())
            {
                std::string import_lib = cls->get_import_lib();

                // Convert IDL import to .proto import
                // If the import is an IDL file, convert it to .proto extension
                if (import_lib.find(".idl") != std::string::npos)
                {
                    // Replace .idl with /protobuf/{filename}.proto
                    // Import the master .proto file which itself imports all namespace files
                    std::string proto_import = import_lib;
                    size_t pos = proto_import.find(".idl");
                    if (pos != std::string::npos)
                    {
                        // Extract the directory and filename
                        size_t last_slash = proto_import.rfind('/', pos);
                        std::string dir_part;
                        std::string file_part;

                        if (last_slash != std::string::npos)
                        {
                            dir_part = proto_import.substr(0, last_slash);
                            file_part = proto_import.substr(last_slash + 1, pos - last_slash - 1);
                        }
                        else
                        {
                            file_part = proto_import.substr(0, pos);
                        }

                        // Construct the new path: dir/protobuf/file_all.proto (master aggregator file)
                        // Import the master aggregator file (lightweight, no dummy messages)
                        if (!dir_part.empty())
                        {
                            proto_import = dir_part + "/protobuf/" + file_part + "_all.proto";
                        }
                        else
                        {
                            proto_import = "protobuf/" + file_part + "_all.proto";
                        }
                    }

                    // Make sure the path is relative and properly formatted
                    // Remove leading slashes if present
                    if (!proto_import.empty() && proto_import[0] == '/')
                    {
                        proto_import = proto_import.substr(1);
                    }

                    unique_imports.insert(proto_import);
                }
                else
                {
                    // If it's not an IDL file, assume it's a standard proto import
                    unique_imports.insert(import_lib);
                }
            }
        }

        // Write all unique imports
        for (const auto& import : unique_imports)
        {
            proto("import \"{}\";", import);
        }

        if (!unique_imports.empty())
        {
            proto("");
        }
    }

    // Helper function to collect template instantiations from a class/namespace (recursive)
    void collect_template_instantiations(const class_entity& lib, std::set<TemplateInstantiation>& instantiations)
    {
        // Scan all interfaces for template usage
        for (auto& interface_elem : lib.get_elements(entity_type::INTERFACE))
        {
            auto& interface_entity = static_cast<const class_entity&>(*interface_elem);

            for (auto& function : interface_entity.get_functions())
            {
                if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
                {
                    // Check parameters for template instantiations
                    for (auto& parameter : function->get_parameters())
                    {
                        std::string param_type = parameter.get_type();

                        // Check if it's an [in] parameter (explicit or implicit)
                        bool is_in = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                        if (!is_in)
                            continue;

                        // Check if it's a template instantiation (has < and >)
                        size_t template_start = param_type.find('<');
                        if (template_start != std::string::npos && param_type.find('>') != std::string::npos)
                        {
                            std::string template_name = param_type.substr(0, template_start);

                            // Skip std:: and rpc:: types - only user-defined templates
                            if (template_name.find("std::") == 0 || template_name.find("rpc::") == 0)
                                continue;

                            // Extract template parameter
                            int bracket_count = 1;
                            size_t pos = template_start + 1;
                            while (pos < param_type.length() && bracket_count > 0)
                            {
                                if (param_type[pos] == '<')
                                    bracket_count++;
                                else if (param_type[pos] == '>')
                                    bracket_count--;
                                pos++;
                            }

                            if (bracket_count == 0)
                            {
                                std::string template_param
                                    = param_type.substr(template_start + 1, pos - template_start - 2);
                                std::string concrete_name = cpp_type_to_proto_type(param_type);

                                TemplateInstantiation inst;
                                inst.template_name = template_name;
                                inst.template_param = template_param;
                                inst.concrete_name = concrete_name;
                                instantiations.insert(inst);
                            }
                        }
                    }
                }
            }
        }

        // Scan non-template struct fields for template type usages (e.g. test_template<int> as a field)
        for (auto& struct_elem : lib.get_elements(entity_type::STRUCT))
        {
            auto& struct_entity = static_cast<const class_entity&>(*struct_elem);
            if (struct_entity.get_is_template())
                continue;

            for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
            {
                if (member->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;

                auto func_entity = std::static_pointer_cast<function_entity>(member);
                if (func_entity->is_static())
                    continue;

                std::string field_type = func_entity->get_return_type();
                size_t template_start = field_type.find('<');
                if (template_start == std::string::npos || field_type.find('>') == std::string::npos)
                    continue;

                std::string template_name = field_type.substr(0, template_start);
                // Skip std:: and rpc:: container types — only user-defined templates
                if (template_name.find("std::") == 0 || template_name.find("rpc::") == 0)
                    continue;

                // Extract template parameter
                int bracket_count = 1;
                size_t pos = template_start + 1;
                while (pos < field_type.length() && bracket_count > 0)
                {
                    if (field_type[pos] == '<')
                        bracket_count++;
                    else if (field_type[pos] == '>')
                        bracket_count--;
                    pos++;
                }

                if (bracket_count == 0)
                {
                    std::string template_param = field_type.substr(template_start + 1, pos - template_start - 2);
                    std::string concrete_name = cpp_type_to_proto_type(field_type);

                    TemplateInstantiation inst;
                    inst.template_name = template_name;
                    inst.template_param = template_param;
                    inst.concrete_name = concrete_name;
                    instantiations.insert(inst);
                }
            }
        }

        // Recursively process nested namespaces
        for (auto& ns_elem : lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (ns_elem->get_entity_type() == entity_type::NAMESPACE)
            {
                auto& ns_entity = static_cast<const class_entity&>(*ns_elem);
                collect_template_instantiations(ns_entity, instantiations);
            }
        }
    }

    // Operator for set comparison of TemplateInstantiation
    bool operator<(const TemplateInstantiation& a, const TemplateInstantiation& b)
    {
        return a.concrete_name < b.concrete_name;
    }

    // Process a namespace and its contents
    void write_namespace(bool from_host,
        const class_entity& lib,
        std::string prefix,
        writer& proto,
        bool catch_stub_exceptions,
        const std::vector<std::string>& rethrow_exceptions)
    {
        for (auto& elem : lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->is_in_import())
                continue;
            else if (elem->get_entity_type() == entity_type::NAMESPACE)
            {
                bool is_inline = elem->has_value("inline");

                // For protobuf, we don't use inline namespaces, just regular package structure
                if (!is_inline)
                {
                    // In protobuf, nested messages can be defined within other messages
                    // For now, we'll process the namespace contents directly
                    auto& ent = static_cast<const class_entity&>(*elem);
                    write_namespace(
                        from_host, ent, prefix + elem->get_name() + ".", proto, catch_stub_exceptions, rethrow_exceptions);
                }
                else
                {
                    // For inline namespaces, just process contents directly
                    auto& ent = static_cast<const class_entity&>(*elem);
                    write_namespace(from_host, ent, prefix, proto, catch_stub_exceptions, rethrow_exceptions);
                }
            }
            else if (elem->get_entity_type() == entity_type::STRUCT)
            {
                // Skip template struct definitions - only concrete instantiations should be generated
                auto& struct_entity = static_cast<const class_entity&>(*elem);
                if (!struct_entity.get_is_template())
                {
                    // Generate message definition for non-template struct
                    int field_number = 0;
                    write_message_definition(struct_entity, proto, field_number);
                }
            }
            else if (elem->get_entity_type() == entity_type::ENUM)
            {
                // Generate enum definition for enum
                auto& enum_entity = static_cast<const class_entity&>(*elem);
                std::string enum_name = sanitize_type_name(enum_entity.get_name());
                proto("enum {} {{", enum_name);

                // Get enum values from the functions list
                auto enum_vals = enum_entity.get_functions();

                // In protobuf3, the first enum value MUST be 0
                // Check if any value has explicit value 0
                bool has_zero_value = false;
                for (auto& enum_val : enum_vals)
                {
                    if (!enum_val->get_return_type().empty() && enum_val->get_return_type() == "0")
                    {
                        has_zero_value = true;
                        break;
                    }
                }

                // If no zero value exists, add UNSPECIFIED = 0
                if (!has_zero_value && !enum_vals.empty())
                {
                    // Check if first value is not zero (enum_vals is a std::list, so use iterator)
                    auto first_val = enum_vals.begin();
                    if (first_val != enum_vals.end())
                    {
                        if (!(*first_val)->get_return_type().empty() && (*first_val)->get_return_type() != "0")
                        {
                            proto("{}_UNSPECIFIED = 0;", enum_name);
                        }
                    }
                }

                int enum_counter = 0;
                for (auto& enum_val : enum_vals)
                {
                    // Prefix enum values with enum type name to avoid collisions in protobuf3
                    // In proto3, enum values are scoped to the package, not the enum
                    std::string prefixed_name = enum_name + "_" + sanitize_type_name(enum_val->get_name());

                    if (enum_val->get_return_type().empty())
                    {
                        // Enum value without explicit value
                        proto("{} = {};", prefixed_name, enum_counter++);
                    }
                    else
                    {
                        // Enum value with explicit value
                        proto("{} = {};", prefixed_name, enum_val->get_return_type());
                    }
                }

                proto("}}");
                proto("");
            }
            // Skip interface processing in namespace files since interfaces will be in separate files
        }
    }

    // Returns true if any non-static field in any non-template struct in current_lib is __int128.
    // Used to decide whether to emit the uint128 helper message into the proto file.
    bool namespace_has_int128_fields(const class_entity& current_lib)
    {
        for (auto& elem : current_lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->get_entity_type() == entity_type::STRUCT)
            {
                auto& struct_entity = static_cast<const class_entity&>(*elem);
                if (struct_entity.get_is_template())
                    continue;
                for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
                {
                    if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
                    {
                        auto func_entity = std::static_pointer_cast<function_entity>(member);
                        if (func_entity->is_static())
                            continue;
                        const std::string& t = func_entity->get_return_type();
                        if (t == "unsigned __int128" || t == "__int128" || t == "uint128_t" || t == "int128_t")
                            return true;
                        // Also detect int128 inside vector/array container fields.
                        // Inline the type check here to avoid forward-reference dependency on later helpers.
                        auto inner_is_int128 = [](const std::string& inner)
                        {
                            return inner == "__int128" || inner == "unsigned __int128" || inner == "int128_t"
                                   || inner == "uint128_t";
                        };
                        auto extract_first_template_arg = [](const std::string& type) -> std::string
                        {
                            size_t lt = type.find('<');
                            size_t gt = type.rfind('>');
                            if (lt == std::string::npos || gt == std::string::npos || gt <= lt)
                                return "";
                            std::string inner = type.substr(lt + 1, gt - lt - 1);
                            // For array<T, N> take only the part before the first comma
                            size_t comma = inner.find(',');
                            if (comma != std::string::npos)
                                inner = inner.substr(0, comma);
                            // Trim spaces
                            size_t f = inner.find_first_not_of(' ');
                            size_t l = inner.find_last_not_of(' ');
                            return (f == std::string::npos) ? "" : inner.substr(f, l - f + 1);
                        };
                        if ((t.find("std::vector<") == 0 || t.find("std::array<") == 0)
                            && inner_is_int128(extract_first_template_arg(t)))
                            return true;
                    }
                }
            }
        }
        return false;
    }

    // Helper function to write a single namespace to a file
    void write_single_namespace(const class_entity& lib,
        const class_entity& current_lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& sub_directory,
        std::vector<std::string>& generated_files)
    {

        // Create the file path for this namespace with full prefix
        // sub_directory already includes "/protobuf" suffix
        // Add schema/ subdirectory for all proto files
        std::string namespace_name = get_namespace_name(current_lib);

        // Skip empty namespace names (root namespace without a name)
        if (namespace_name.empty())
        {
            // Process children but don't create a file for this empty namespace
            for (auto& elem : current_lib.get_elements(entity_type::NAMESPACE_MEMBERS))
            {
                if (elem->is_in_import())
                    continue;
                if (elem->get_entity_type() == entity_type::NAMESPACE)
                {
                    protobuf_generator::write_single_namespace(
                        lib, static_cast<const class_entity&>(*elem), output_path, sub_directory, generated_files);
                }
            }
            return;
        }

        std::string namespace_filename = namespace_name + ".proto";
        auto proto_dir = output_path / "src" / sub_directory / "schema";
        std::filesystem::create_directories(proto_dir);
        std::string full_path = (proto_dir / namespace_filename).string();

        std::ofstream namespace_file(full_path);
        writer proto(namespace_file);

        // Write the protobuf syntax declaration
        proto("syntax = \"proto3\";");
        proto("");

        // Write imports based on IDL imports
        write_imports(lib, proto);

        // Check if we need to import rpc.proto for interface_descriptor
        bool has_interface_parameters = false;
        for (auto& interface_elem : current_lib.get_elements(entity_type::INTERFACE))
        {
            auto& interface_entity = static_cast<const class_entity&>(*interface_elem);
            for (auto& function : interface_entity.get_functions())
            {
                if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
                {
                    for (auto& parameter : function->get_parameters())
                    {
                        bool optimistic = false;
                        std::shared_ptr<class_entity> obj;
                        if (is_interface_param(interface_entity, parameter.get_type(), optimistic, obj))
                        {
                            has_interface_parameters = true;
                            break;
                        }
                    }
                    if (has_interface_parameters)
                        break;
                }
            }
            if (has_interface_parameters)
                break;
        }

        // Import rpc.proto if we use interface_descriptor
        if (has_interface_parameters)
        {
            proto("import \"rpc/protobuf/schema/rpc.proto\";");
            proto("");
        }

        // Write package declaration with protobuf prefix to avoid namespace collision
        // If the C++ namespace is xxx, the protobuf package will be protobuf.xxx
        proto("package protobuf.{};", namespace_name);
        proto("");

        // Emit the uint128 helper message when any struct field uses a 128-bit integer type.
        // Protobuf has no native 128-bit type; we represent it as two uint64 halves.
        if (namespace_has_int128_fields(current_lib))
        {
            proto("// 128-bit integer represented as two 64-bit halves (lo = bits 0-63, hi = bits 64-127).");
            proto("message uint128 {{");
            proto("\tuint64 lo = 1;");
            proto("\tuint64 hi = 2;");
            proto("}}");
            proto("");
        }

        // First, collect all interface types that are referenced by this interface
        // Also track which packages are referenced for cross-package imports
        std::set<std::shared_ptr<class_entity>> referenced_interfaces;
        std::set<std::shared_ptr<class_entity>> referenced_packages;

        // Collect template instantiations from this namespace
        std::set<TemplateInstantiation> template_instantiations;
        collect_template_instantiations(current_lib, template_instantiations);

        // Generate concrete template message definitions
        for (const auto& inst : template_instantiations)
        {
            // Find the template entity in current_lib
            for (auto& struct_elem : current_lib.get_elements(entity_type::STRUCT))
            {
                auto& struct_entity = static_cast<const class_entity&>(*struct_elem);
                if (struct_entity.get_is_template() && struct_entity.get_name() == inst.template_name)
                {
                    int field_number = 0;
                    write_template_instantiation(
                        struct_entity, inst.template_param, inst.concrete_name, proto, field_number);
                    break;
                }
            }
        }

        // Process only the contents of this specific namespace
        for (auto& elem : current_lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->is_in_import())
                continue;
            else if (elem->get_entity_type() == entity_type::NAMESPACE)
            {
                protobuf_generator::write_single_namespace(
                    lib, static_cast<const class_entity&>(*elem), output_path, sub_directory, generated_files);
            }
            else if (elem->get_entity_type() == entity_type::STRUCT)
            {
                // Skip template struct definitions - only concrete instantiations should be generated
                auto& struct_entity = static_cast<const class_entity&>(*elem);
                if (!struct_entity.get_is_template())
                {
                    // Generate message definition for non-template struct
                    int field_number = 0;
                    write_message_definition(struct_entity, proto, field_number);
                }
            }
            else if (elem->get_entity_type() == entity_type::ENUM)
            {
                // Generate enum definition for enum
                auto& enum_entity = static_cast<const class_entity&>(*elem);
                std::string enum_name = sanitize_type_name(enum_entity.get_name());
                proto("enum {} {{", enum_name);

                // Get enum values from the functions list
                auto enum_vals = enum_entity.get_functions();

                // In protobuf3, the first enum value MUST be 0
                // Check if any value has explicit value 0
                bool has_zero_value = false;
                for (auto& enum_val : enum_vals)
                {
                    if (!enum_val->get_return_type().empty() && enum_val->get_return_type() == "0")
                    {
                        has_zero_value = true;
                        break;
                    }
                }

                // If no zero value exists, add UNSPECIFIED = 0
                if (!has_zero_value && !enum_vals.empty())
                {
                    // Check if first value is not zero (enum_vals is a std::list, so use iterator)
                    auto first_val = enum_vals.begin();
                    if (first_val != enum_vals.end())
                    {
                        if (!(*first_val)->get_return_type().empty() && (*first_val)->get_return_type() != "0")
                        {
                            proto("{}_UNSPECIFIED = 0;", enum_name);
                        }
                    }
                }

                int enum_counter = 0;
                for (auto& enum_val : enum_vals)
                {
                    // Prefix enum values with enum type name to avoid collisions in protobuf3
                    // In proto3, enum values are scoped to the package, not the enum
                    std::string prefixed_name = enum_name + "_" + sanitize_type_name(enum_val->get_name());

                    if (enum_val->get_return_type().empty())
                    {
                        // Enum value without explicit value
                        proto("{} = {};", prefixed_name, enum_counter++);
                    }
                    else
                    {
                        // Enum value with explicit value
                        proto("{} = {};", prefixed_name, enum_val->get_return_type());
                    }
                }

                proto("}}");
                proto("");
            }
            else if (elem->get_entity_type() == entity_type::INTERFACE)
            {
                // Generate a separate .proto file for each interface service in schema/ subdirectory
                auto interface_entity = std::static_pointer_cast<class_entity>(elem);

                // Generate the service file (contains the request/response messages and service definition)
                write_single_interface_service(lib, interface_entity, proto, referenced_interfaces, referenced_packages);
            }
        }

        // Add the namespace file to generated_files ONCE after processing all elements
        // Only add if this namespace file actually contains structs/enums (not just interfaces)
        std::filesystem::path schema_file = sub_directory / "schema" / (namespace_name + ".proto");
        // Check if this file was already added or if we should add it
        if (std::find(generated_files.begin(), generated_files.end(), schema_file.string()) == generated_files.end())
        {
            generated_files.push_back(schema_file.string());
        }
    }

    // Helper function to write a single interface service to a file
    void write_single_interface_service(const class_entity& lib,
        const std::shared_ptr<class_entity>& interface_entity,
        writer& proto,
        std::set<std::shared_ptr<class_entity>>& referenced_interfaces,
        std::set<std::shared_ptr<class_entity>>& referenced_packages)
    {
        std::string interface_name = interface_entity->get_name();

        auto ns = interface_entity->get_owner();
        while (ns && ns->get_name().empty())
        {
            interface_name = ns->get_name() + "_" + interface_name;
            ns = ns->get_owner();
        }

        for (auto& function : interface_entity->get_functions())
        {
            if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
            {
                // Check input parameters
                for (auto& parameter : function->get_parameters())
                {
                    // Check if the parameter type is an interface
                    bool optimistic = false;
                    std::shared_ptr<class_entity> obj;
                    if (is_interface_param(*interface_entity, parameter.get_type(), optimistic, obj))
                    {
                        referenced_interfaces.insert(obj);
                    }
                    else
                    {
                        std::string reference_modifiers;
                        std::string type_name = parameter.get_type();
                        strip_reference_modifiers(type_name, reference_modifiers);

                        if (lib.find_class(type_name, obj))
                        {
                            referenced_packages.insert(obj);
                        }
                    }
                }
            }
        }

        // Remove this interface from the referenced interfaces (we'll define it locally after the service)
        referenced_interfaces.erase(interface_entity);

        // Import pointer files from imported namespaces
        // Only import if the imported module has interfaces (check if it's an interface type)
        // Also build a map of package names to their pointer file paths
        // std::set<std::string> imported_pointer_files;
        std::map<std::string, std::string> package_to_pointer_file;

        // Recursively scan all elements to find imported interfaces
        // std::function<void(const class_entity&)> scan_for_imports = [&](const class_entity& entity)
        // {
        //     // Check interfaces in this entity
        //     for (auto& elem : entity.get_elements(entity_type::INTERFACE))
        //     {
        //         auto& interface_cls = static_cast<const class_entity&>(*elem);
        //         std::string import_lib = interface_cls.get_import_lib();

        //         if (!import_lib.empty())
        //         {
        //             std::string cls_package = interface_cls.get_owner() ? interface_cls.get_owner()->get_name() : "";

        //             // Convert IDL import to pointer proto import
        //             if (import_lib.find(".idl") != std::string::npos)
        //             {
        //                 // Extract directory and filename
        //                 size_t pos = import_lib.find(".idl");
        //                 size_t last_slash = import_lib.rfind('/', pos);
        //                 std::string dir_part, file_part;

        //                 if (last_slash != std::string::npos)
        //                 {
        //                     dir_part = import_lib.substr(0, last_slash);
        //                     file_part = import_lib.substr(last_slash + 1, pos - last_slash - 1);
        //                 }
        //                 else
        //                 {
        //                     file_part = import_lib.substr(0, pos);
        //                 }

        //                 // Construct path to the imported module's _all.proto file (contains pointers)
        //                 std::string imported_all_proto;
        //                 if (!dir_part.empty())
        //                 {
        //                     imported_all_proto = dir_part + "/protobuf/" + file_part + "_all.proto";
        //                 }
        //                 else
        //                 {
        //                     imported_all_proto = "protobuf/" + file_part + "_all.proto";
        //                 }

        //                 // Remove leading slashes
        //                 if (!imported_all_proto.empty() && imported_all_proto[0] == '/')
        //                 {
        //                     imported_all_proto = imported_all_proto.substr(1);
        //                 }

        //                 imported_pointer_files.insert(imported_all_proto);

        //                 // Map the package name to its _all.proto file for later lookup
        //                 if (!cls_package.empty())
        //                 {
        //                     package_to_pointer_file[cls_package] = imported_all_proto;
        //                 }
        //             }
        //         }
        //     }

        //     // Recursively scan namespaces
        //     for (auto& elem : entity.get_elements(entity_type::NAMESPACE))
        //     {
        //         auto& ns_entity = static_cast<const class_entity&>(*elem);
        //         scan_for_imports(ns_entity);
        //     }
        // };

        // // Start the recursive scan from the root lib entity
        // scan_for_imports(lib);

        // // Write imports for pointer files from imported modules
        // for (const auto& imported_ptr_file : imported_pointer_files)
        // {
        //     proto("import \"{}\";", imported_ptr_file);
        // }

        // // Import pointer files for cross-package referenced types
        // // Use the package_to_pointer_file map built above
        // for (const auto& pkg : referenced_packages)
        // {
        //     // Skip if this is the current package
        //     if (pkg == package_name)
        //         continue;

        //     // Find the pointer file for this package
        //     auto it = package_to_pointer_file.find(pkg);
        //     if (it != package_to_pointer_file.end())
        //     {
        //         std::string pointer_file = it->second;

        //         // Avoid duplicates (already written above)
        //         if (imported_pointer_files.find(pointer_file) == imported_pointer_files.end())
        //         {
        //             proto("import \"{}\";", pointer_file);
        //         }
        //     }
        // }

        // NOTE: Previously generated per-interface _ptr message definitions here,
        // but now we use the unified rpc.interface_descriptor type instead
        std::set<std::string> defined_messages;

        // Generate request/response messages grouped before the service (Google-style organization)
        // Services cannot contain nested messages, but we group them logically before the service
        proto("// ===== {} Service Messages =====", interface_name);
        proto("");

        for (auto& function : interface_entity->get_functions())
        {
            if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
            {
                std::string method_name = function->get_name();
                std::string input_type = interface_name + "_" + method_name + "Request";
                std::string output_type = interface_name + "_" + method_name + "Response";

                // Write request message
                proto("message {} {{", input_type);
                int field_number = 0;
                for (auto& parameter : function->get_parameters())
                {
                    // Treat parameters without [in] or [out] as implicit [in] parameters
                    bool is_in = is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter));
                    if (is_in)
                    {
                        std::string param_type;
                        std::string param_name = sanitize_field_name(parameter.get_name());

                        bool optimistic = false;
                        std::shared_ptr<class_entity> obj;
                        // For interface types, use the unified interface_descriptor
                        if (is_interface_param(*interface_entity, parameter.get_type(), optimistic, obj))
                        {
                            param_type = "rpc.interface_descriptor";
                        }
                        else
                        {
                            param_type = cpp_type_to_proto_type(parameter.get_type());
                            // For custom types, sanitize the name
                            if (param_type.find("::") != std::string::npos)
                            {
                                param_type = sanitize_type_name(param_type);
                            }
                        }

                        field_number++;
                        proto("{} {} = {};", param_type, param_name, field_number);
                    }
                }
                proto("}}");
                proto("");

                // Write response message
                proto("message {} {{", output_type);
                field_number = 0;
                for (auto& parameter : function->get_parameters())
                {
                    if (is_out_param(parameter))
                    {
                        std::string param_type;
                        std::string param_name = sanitize_field_name(parameter.get_name());

                        // For interface types, use the unified interface_descriptor
                        bool optimistic = false;
                        std::shared_ptr<class_entity> obj;
                        if (is_interface_param(lib, parameter.get_type(), optimistic, obj))
                        {
                            param_type = "rpc.interface_descriptor";
                        }
                        else
                        {
                            param_type = cpp_type_to_proto_type(parameter.get_type());
                            // For custom types, sanitize the name
                            if (param_type.find("::") != std::string::npos)
                            {
                                param_type = sanitize_type_name(param_type);
                            }
                        }

                        field_number++;
                        proto("{} {} = {};", param_type, param_name, field_number);
                    }
                }

                // If there's a return type, add it to the response
                if (!function->get_return_type().empty() && function->get_return_type() != "void")
                {
                    std::string return_type = cpp_type_to_proto_type(function->get_return_type());
                    if (return_type.find("::") != std::string::npos)
                    {
                        return_type = sanitize_type_name(return_type);
                    }

                    field_number++;
                    proto("{} result = {};", return_type, field_number);
                }

                proto("}}");
                proto("");
            }
        }

        // Define the service with RPC method declarations
        proto("// ===== {} Service =====", interface_name);
        proto("service {} {{", interface_name);
        for (auto& function : interface_entity->get_functions())
        {
            if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
            {
                std::string method_name = function->get_name();
                std::string input_type = interface_name + "_" + method_name + "Request";
                std::string output_type = interface_name + "_" + method_name + "Response";

                // Write the RPC method declaration
                proto("rpc {}({}) returns ({});", method_name, input_type, output_type);
            }
        }
        proto("}}");
        proto("");
    }

    // entry point - generates multiple files for nested namespaces
    // Returns a vector of generated .proto file paths (relative to output_path/src)
    std::vector<std::string> write_files(const class_entity& lib,
        const std::filesystem::path& output_path,
        const std::filesystem::path& sub_directory,
        const std::filesystem::path& base_filename)
    {
        // Collect all namespace names and interfaces from the IDL
        std::set<std::string> namespace_names;
        std::vector<std::shared_ptr<entity>> interfaces;

        // Start the recursive collection from the root
        // collect_from_namespace(lib);

        // If there are no namespaces in the IDL, create a default one
        if (namespace_names.empty())
        {
            namespace_names.insert("default");
        }

        // Extract module name from base_filename for prefixing
        std::string module_name = base_filename.filename().string();

        // Generate a separate .proto file for each namespace in schema/ subdirectory
        std::vector<std::string> generated_files;
        protobuf_generator::write_single_namespace(lib, lib, output_path, sub_directory, generated_files);

        // Create master .proto file that imports other proto files AND contains pointer definitions
        // This provides a single import point for cross-IDL references and interface pointers
        std::string master_filename = base_filename.filename().string() + "_all.proto";
        auto proto_dir = std::filesystem::path(output_path) / "src" / sub_directory;
        std::filesystem::create_directories(proto_dir);
        std::string master_full_path = (proto_dir / master_filename).string();

        std::ofstream master_file(master_full_path);
        writer master_proto(master_file);

        // Write the protobuf syntax declaration
        master_proto("syntax = \"proto3\";");
        master_proto("");

        // Write imports from external IDL dependencies
        write_imports(lib, master_proto);

        // Deduplicate generated_files to avoid duplicate imports
        std::set<std::string> unique_generated_files(generated_files.begin(), generated_files.end());

        // Import all the individual namespace and interface files using "public import"
        // Import paths must be relative to PROTO_SRC_DIR for cross-module imports to work
        std::string master_relative_path = (sub_directory / master_filename).string();
        for (const auto& gen_file : unique_generated_files)
        {
            // Skip if this generated file is the master file itself
            if (gen_file == master_relative_path)
                continue;

            // Skip empty or invalid paths
            if (gen_file.empty() || gen_file.find("/.proto") != std::string::npos)
                continue;

            // Use the full path relative to PROTO_SRC_DIR for cross-module compatibility
            // e.g., "rpc/protobuf/schema/rpc.proto" stays as-is
            master_proto("import public \"{}\";", gen_file);
        }
        master_proto("");

        // NOTE: Pointer definitions are NOW in each interface file (in schema/)
        // This avoids circular imports and allows each interface to define its own pointers
        // and any pointers it references

        // Generate a manifest.txt file listing all .proto files for CMake dependency tracking
        std::string manifest_full_path = (proto_dir / "manifest.txt").string();

        // Open manifest file for writing
        std::ofstream manifest_file(manifest_full_path);
        writer manifest(manifest_file);

        // Add the master .proto file first (in protobuf/ directory)
        // Use full path from PROTO_SRC_DIR for consistency with cross-module imports
        std::string master_relative_to_src = (sub_directory / master_filename).string();
        manifest("{}", master_relative_to_src);

        // Add all individual .proto files that were generated
        // Use full paths relative to PROTO_SRC_DIR (e.g., "rpc/protobuf/schema/rpc.proto")
        for (const auto& gen_file : generated_files)
        {
            manifest("{}", gen_file);
        }

        // Return the list of generated .proto file paths for C++ wrapper includes
        return generated_files;
    }

    // Helper function to check if a type is a primitive type (not a struct/class/interface)
    // Helper to normalize type string by removing qualifiers
    std::string normalize_type(const std::string& type_str)
    {
        std::string cleaned_type = type_str;

        // Remove const
        size_t const_pos = cleaned_type.find("const ");
        while (const_pos != std::string::npos)
        {
            cleaned_type.erase(const_pos, 6);
            const_pos = cleaned_type.find("const ");
        }

        // Remove trailing &, &&, *, and spaces
        while (!cleaned_type.empty()
               && (cleaned_type.back() == '&' || cleaned_type.back() == '*' || cleaned_type.back() == ' '))
        {
            cleaned_type.pop_back();
        }

        // Remove leading spaces
        while (!cleaned_type.empty() && cleaned_type.front() == ' ')
        {
            cleaned_type.erase(0, 1);
        }

        return cleaned_type;
    }

    bool is_primitive_type(const std::string& type_str)
    {
        // Remove const, reference, pointer qualifiers
        std::string cleaned_type = type_str;

        // Remove const
        if (cleaned_type.find("const ") == 0)
            cleaned_type = cleaned_type.substr(6);

        // Remove trailing &, &&, *
        while (!cleaned_type.empty()
               && (cleaned_type.back() == '&' || cleaned_type.back() == '*' || cleaned_type.back() == ' '))
        {
            cleaned_type.pop_back();
        }

        // Pointers and interfaces are NOT primitives
        if (type_str.find('*') != std::string::npos || type_str.find("rpc::shared_ptr") != std::string::npos
            || type_str.find("rpc::interface_descriptor") != std::string::npos
            || type_str.find("std::vector") != std::string::npos || type_str.find("std::map") != std::string::npos)
        {
            return false;
        }

        // Check for primitive types
        static const std::set<std::string> primitives = {"int",
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
            "wchar_t",
            "bool",
            "float",
            "double",
            "long double",
            "size_t",
            "ptrdiff_t",
            "error_code"};

        return primitives.find(cleaned_type) != primitives.end();
    }

    // Helper to check if type is protobuf-serializable as primitive/simple type
    bool is_simple_protobuf_type(const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);

        // std::string is a simple protobuf type
        if (norm_type == "std::string")
            return true;

        // std::vector<uint8_t> maps to protobuf bytes
        if (norm_type == "std::vector<uint8_t>" || norm_type == "std::vector<unsignedchar>")
            return true;

        // Check for std::vector<T> where T is a primitive type
        if (norm_type.find("std::vector<") == 0)
        {
            size_t start = std::string("std::vector<").length();
            size_t end = norm_type.rfind('>');
            if (end != std::string::npos && end > start)
            {
                std::string inner_type = norm_type.substr(start, end - start);
                if (is_primitive_type(inner_type) || inner_type == "std::string")
                    return true;
            }
        }

        // Check for std::map<K, V>, std::unordered_map<K, V>, std::flat_map<K, V>
        // where K and V are primitive types or std::string
        std::string map_prefix;
        if (norm_type.find("std::map<") == 0)
            map_prefix = "std::map<";
        else if (norm_type.find("std::unordered_map<") == 0)
            map_prefix = "std::unordered_map<";
        else if (norm_type.find("std::flat_map<") == 0)
            map_prefix = "std::flat_map<";

        if (!map_prefix.empty())
        {
            size_t start = map_prefix.length();
            size_t end = norm_type.rfind('>');
            if (end != std::string::npos && end > start)
            {
                std::string inner = norm_type.substr(start, end - start);
                // Split at comma (simple split, doesn't handle nested templates in map values)
                size_t comma = inner.find(',');
                if (comma != std::string::npos)
                {
                    std::string key_type = inner.substr(0, comma);
                    std::string value_type = inner.substr(comma + 1);
                    // Trim whitespace
                    while (!key_type.empty() && key_type.back() == ' ')
                        key_type.pop_back();
                    while (!value_type.empty() && value_type.front() == ' ')
                        value_type.erase(0, 1);

                    bool key_ok = is_primitive_type(key_type) || key_type == "std::string";
                    bool value_ok = is_primitive_type(value_type) || value_type == "std::string";
                    if (key_ok && value_ok)
                        return true;
                }
            }
        }

        // Primitive types
        return is_primitive_type(norm_type);
    }

    // Helper to check if type is a vector with scalar elements
    bool is_scalar_vector_type(const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);
        if (norm_type.find("std::vector<") != 0)
            return false;

        // Exclude byte vectors (handled separately as bytes)
        if (norm_type == "std::vector<uint8_t>" || norm_type == "std::vector<unsignedchar>"
            || norm_type == "std::vector<char>" || norm_type == "std::vector<signedchar>")
            return false;

        size_t start = std::string("std::vector<").length();
        size_t end = norm_type.rfind('>');
        if (end != std::string::npos && end > start)
        {
            std::string inner_type = norm_type.substr(start, end - start);
            return is_primitive_type(inner_type) || inner_type == "std::string";
        }
        return false;
    }

    // Helper to check if type is a map with scalar key and value
    bool is_scalar_map_type(const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);

        std::string map_prefix;
        if (norm_type.find("std::map<") == 0)
            map_prefix = "std::map<";
        else if (norm_type.find("std::unordered_map<") == 0)
            map_prefix = "std::unordered_map<";
        else if (norm_type.find("std::flat_map<") == 0)
            map_prefix = "std::flat_map<";
        else
            return false;

        size_t start = map_prefix.length();
        size_t end = norm_type.rfind('>');
        if (end != std::string::npos && end > start)
        {
            std::string inner = norm_type.substr(start, end - start);
            size_t comma = inner.find(',');
            if (comma != std::string::npos)
            {
                std::string key_type = inner.substr(0, comma);
                std::string value_type = inner.substr(comma + 1);
                while (!key_type.empty() && key_type.back() == ' ')
                    key_type.pop_back();
                while (!value_type.empty() && value_type.front() == ' ')
                    value_type.erase(0, 1);

                bool key_ok = is_primitive_type(key_type) || key_type == "std::string";
                bool value_ok = is_primitive_type(value_type) || value_type == "std::string";
                return key_ok && value_ok;
            }
        }
        return false;
    }

    // Helper to check if a type is an enum defined in the IDL
    bool is_enum_type(const class_entity& lib, const std::string& type_str)
    {
        std::string norm_type = normalize_type(type_str);

        // Extract unqualified name for cross-namespace lookup (e.g. "rpc::encoding" -> "encoding")
        std::string unqualified = norm_type;
        size_t ns_pos = norm_type.rfind("::");
        if (ns_pos != std::string::npos)
            unqualified = norm_type.substr(ns_pos + 2);

        // Recursively search for enum with this name
        std::function<bool(const class_entity&)> search_for_enum = [&](const class_entity& entity) -> bool
        {
            // Check enums in this entity
            for (auto& elem : entity.get_elements(entity_type::ENUM))
            {
                if (elem->get_name() == norm_type || elem->get_name() == unqualified)
                    return true;
            }

            // Check nested namespaces
            for (auto& elem : entity.get_elements(entity_type::NAMESPACE))
            {
                auto& ns_entity = static_cast<const class_entity&>(*elem);
                if (search_for_enum(ns_entity))
                    return true;
            }

            return false;
        };

        return search_for_enum(lib);
    }

    // Helper to write a single proxy serialization function for protobuf
    void write_proxy_protobuf_method(const class_entity& lib,
        const class_entity& interface_entity,
        const std::shared_ptr<function_entity>& function,
        const std::string& interface_name,
        const std::string& package_name,
        writer& cpp)
    {
        (void)interface_entity; // unused but kept for consistency with other generators
        std::string function_name = function->get_name();

        // Generate function signature
        cpp("template<>");
        cpp("int {}::proxy_serialiser<rpc::serialiser::protocol_buffers>::{}(", interface_name, function_name);
        cpp("// Protobuf serialization method");

        // Add parameters and track their types
        std::vector<std::pair<std::string, std::string>> param_info; // (name, type)
        bool first_param = true;
        for (auto& parameter : function->get_parameters())
        {
            if (is_in_param(parameter) || (!is_in_param(parameter) && !is_out_param(parameter)))
            {
                std::string param_type = parameter.get_type();
                std::string param_name = parameter.get_name();
                param_info.push_back({param_name, param_type});

                // Transform parameter type to match proxy_serialiser signature:
                // - IDL pointers (T*) become uint64_t
                // - RPC interfaces (rpc::shared_ptr<T> or rpc::optimistic_ptr<T>) become const rpc::interface_descriptor&
                std::string final_param_type;

                if (param_type.find("rpc::shared_ptr") != std::string::npos
                    || param_type.find("rpc::optimistic_ptr") != std::string::npos)
                {
                    // Interface types become interface_descriptor
                    final_param_type = "const rpc::interface_descriptor&";
                }
                else if (param_type.find('*') != std::string::npos)
                {
                    // Pointers become uint64_t
                    final_param_type = "uint64_t";
                }
                else
                {
                    // Check if type has rvalue reference (&&) or lvalue reference (&)
                    bool has_rvalue_ref = (param_type.find("&&") != std::string::npos);
                    bool has_lvalue_ref = (!has_rvalue_ref && param_type.find('&') != std::string::npos);

                    if (has_rvalue_ref)
                    {
                        std::string base_type = param_type;
                        std::string reference_modifiers;
                        rpc_generator::strip_reference_modifiers(base_type, reference_modifiers);
                        final_param_type = "const " + base_type + "&";
                    }
                    else if (has_lvalue_ref)
                    {
                        // Lvalue reference: add const if not present
                        if (param_type.find("const") == std::string::npos)
                            final_param_type = "const " + param_type;
                        else
                            final_param_type = param_type;
                    }
                    else
                    {
                        // Plain type - use const T& form
                        final_param_type = "const " + param_type + "&";
                    }
                }

                if (!first_param)
                    cpp("{} {},", final_param_type, param_name);
                else
                {
                    cpp("{} {},", final_param_type, param_name);
                    first_param = false;
                }
            }
        }

        // Add buffer and encoding parameters
        cpp("std::vector<char>& __buffer)");
        cpp("{{");

        // Create protobuf message instance with protobuf:: namespace prefix
        std::string request_message = interface_name + "_" + function_name + "Request";
        if (!package_name.empty())
            cpp("protobuf::{}::{} __request;", package_name, request_message);
        else
            cpp("protobuf::{} __request;", request_message);

        // Set message fields from parameters
        for (const auto& [param_name, param_type] : param_info)
        {
            // Check if this is a pointer type (IDL pointers become uint64_t in signatures - marshal address only)
            bool is_pointer = (param_type.find('*') != std::string::npos);
            // Check if this is an rpc::shared_ptr or rpc::optimistic_ptr (becomes interface_descriptor)
            bool is_interface = (param_type.find("rpc::shared_ptr") != std::string::npos
                                 || param_type.find("rpc::optimistic_ptr") != std::string::npos
                                 || param_type.find("rpc::interface_descriptor") != std::string::npos);

            if (is_interface)
            {
                // Interface types need special handling - serialize interface_descriptor to proto message
                cpp("auto* proto_{} = __request.mutable_{}();", param_name, param_name);
                cpp("{{");
                cpp("std::vector<char> __dz_buf;");
                cpp("{}.destination_zone_id.protobuf_serialise(__dz_buf);", param_name);
                cpp("if (!proto_{}->mutable_destination_zone_id()->ParseFromArray(__dz_buf.data(), "
                    "static_cast<int>(__dz_buf.size())))",
                    param_name);
                cpp("throw std::runtime_error(\"Failed to parse nested destination_zone\");");
                cpp("}}");
            }
            else if (is_pointer)
            {
                // Pointer types marshal the address only (uint64_t)
                cpp("__request.set_{}({});", param_name, param_name);
            }
            else if (is_simple_protobuf_type(param_type))
            {
                // Simple protobuf types (primitives, std::string, containers with scalar elements)
                std::string norm_type = normalize_type(param_type);
                if (norm_type == "std::vector<uint8_t>" || norm_type == "std::vector<unsignedchar>")
                {
                    // Use helper for bytes
                    cpp("rpc::serialization::protobuf::serialize_bytes({}, *__request.mutable_{}());", param_name, param_name);
                }
                else if (is_scalar_vector_type(param_type))
                {
                    // Vector of scalar types - copy elements to repeated field
                    cpp("for (const auto& __elem : {}) {{", param_name);
                    cpp("__request.add_{}(__elem);", param_name);
                    cpp("}}");
                }
                else if (is_scalar_map_type(param_type))
                {
                    // Map with scalar key/value - copy to protobuf map
                    cpp("auto* __map = __request.mutable_{}();", param_name);
                    cpp("for (const auto& [__k, __v] : {}) {{", param_name);
                    cpp("(*__map)[__k] = __v;");
                    cpp("}}");
                }
                else
                {
                    // Primitives and std::string
                    cpp("__request.set_{}({});", param_name, param_name);
                }
            }
            else if (is_enum_type(lib, param_type))
            {
                // Enum types can be directly assigned - cast to protobuf enum type
                std::string norm_type = normalize_type(param_type);
                cpp("__request.set_{}(static_cast<protobuf::{}::{}>({}));", param_name, package_name, norm_type, param_name);
            }
            else
            {
                // Complex IDL-defined types (structs, vectors of structs, maps, etc.)
                // Use the struct's protobuf_serialise method
                cpp("// Serialize complex input parameter");
                cpp("{{");
                cpp("std::vector<char> param_buffer;");
                cpp("{}.protobuf_serialise(param_buffer);", param_name);
                cpp("auto* proto_param = __request.mutable_{}();", param_name);
                cpp("(void)proto_param->ParseFromArray(param_buffer.data(), static_cast<int>(param_buffer.size()));");
                cpp("}}");
            }
        }

        // Serialize to buffer
        cpp("__buffer.clear();");
        cpp("__buffer.resize(__request.ByteSizeLong());");
        cpp("if (!__request.SerializeToArray(__buffer.data(), static_cast<int>(__buffer.size())))");
        cpp("{{");
        cpp("return rpc::error::PROXY_DESERIALISATION_ERROR();");
        cpp("}}");
        cpp("return rpc::error::OK();");
        cpp("}}");
        cpp("");
    }

    // Helper to write protobuf deserializer (response parsing) method
    void write_proxy_protobuf_deserializer(const class_entity& lib,
        const class_entity& interface_entity,
        const std::shared_ptr<function_entity>& function,
        const std::string& interface_name,
        const std::string& package_name,
        writer& cpp)
    {
        std::string function_name = function->get_name();

        // Collect output parameters
        std::vector<std::pair<std::string, std::string>> out_params;
        for (auto& param : function->get_parameters())
        {
            if (is_out_param(param))
            {
                std::string param_name = param.get_name();
                std::string param_type = param.get_type();
                out_params.emplace_back(param_name, param_type);
            }
        }

        // Suppress unused parameter warning
        (void)interface_entity;

        // Build deserializer signature with output parameters
        cpp("template<>");
        std::string signature = "int " + interface_name
                                + "::proxy_deserialiser<rpc::serialiser::protocol_buffers>::" + function_name + "(";

        bool first_param = true;
        for (const auto& [param_name, param_type] : out_params)
        {
            if (!first_param)
                signature += ", ";
            first_param = false;

            // Transform output parameter type to match proxy_deserialiser signature
            // Same transformations as input parameters
            std::string final_param_type;
            if (param_type.find("rpc::shared_ptr") != std::string::npos
                || param_type.find("rpc::optimistic_ptr") != std::string::npos)
            {
                // Interface types become interface_descriptor&
                final_param_type = "rpc::interface_descriptor&";
            }
            else if (param_type.find('*') != std::string::npos)
            {
                // Pointers become uint64_t&
                final_param_type = "uint64_t&";
            }
            else if (param_type.find("&&") != std::string::npos)
            {
                // Rvalue references stay as-is
                final_param_type = param_type;
            }
            else if (param_type.find('&') != std::string::npos)
            {
                // Already has &, keep it
                final_param_type = param_type;
            }
            else
            {
                // Plain type, add &
                final_param_type = param_type + "&";
            }

            signature += final_param_type + " " + param_name;
        }

        if (!first_param)
            signature += ", ";
        signature += "const rpc::byte_span& __rpc_data)";
        cpp(signature);
        cpp("{{");

        // Deserialize protobuf response
        std::string response_message = interface_name + "_" + function_name + "Response";
        if (!package_name.empty())
            cpp("protobuf::{}::{} __response;", package_name, response_message);
        else
            cpp("protobuf::{} __response;", response_message);

        cpp("if (!__response.ParseFromArray(__rpc_data.data(), static_cast<int>(__rpc_data.size())))");
        cpp("{{");
        cpp("return rpc::error::PROXY_DESERIALISATION_ERROR();");
        cpp("}}");

        // Only add blank line if we have output parameters
        if (!out_params.empty())
            cpp("");

        // Extract output parameters from __response
        for (const auto& [param_name, param_type] : out_params)
        {
            // Check if this is a pointer type (IDL pointers become uint64_t in signatures - marshal address only)
            bool is_pointer = (param_type.find('*') != std::string::npos);
            // Check if this is an rpc::shared_ptr or rpc::optimistic_ptr (becomes interface_descriptor)
            bool is_interface = (param_type.find("rpc::shared_ptr") != std::string::npos
                                 || param_type.find("rpc::optimistic_ptr") != std::string::npos
                                 || param_type.find("rpc::interface_descriptor") != std::string::npos);

            if (is_interface)
            {
                // Interface types need special handling - deserialize proto message to interface_descriptor
                cpp("const auto& proto_{} = __response.{}();", param_name, param_name);
                cpp("{{");
                cpp("std::vector<char> __dz_buf(proto_{}.destination_zone_id().ByteSizeLong());", param_name);
                cpp("if (!proto_{}.destination_zone_id().SerializeToArray(__dz_buf.data(), "
                    "static_cast<int>(__dz_buf.size())))",
                    param_name);
                cpp("throw std::runtime_error(\"Failed to serialize nested destination_zone\");");
                cpp("{}.destination_zone_id.protobuf_deserialise(__dz_buf);", param_name);
                cpp("}}");
            }
            else if (is_pointer)
            {
                // Pointer types marshal the address only (uint64_t)
                cpp("{} = __response.{}();", param_name, param_name);
            }
            else if (is_simple_protobuf_type(param_type))
            {
                // Simple protobuf types (primitives, std::string, containers with scalar elements)
                std::string norm_type = normalize_type(param_type);
                if (norm_type == "std::vector<uint8_t>" || norm_type == "std::vector<unsignedchar>")
                {
                    // Use helper for bytes
                    cpp("rpc::serialization::protobuf::deserialize_bytes(__response.{}(), {});", param_name, param_name);
                }
                else if (is_scalar_vector_type(param_type))
                {
                    // Vector of scalar types - copy from repeated field
                    cpp("{}.clear();", param_name);
                    cpp("for (int __i = 0; __i < __response.{}_size(); ++__i) {{", param_name);
                    cpp("{}.push_back(__response.{}(__i));", param_name, param_name);
                    cpp("}}");
                }
                else if (is_scalar_map_type(param_type))
                {
                    // Map with scalar key/value - copy from protobuf map
                    cpp("{}.clear();", param_name);
                    cpp("for (const auto& [__k, __v] : __response.{}()) {{", param_name);
                    cpp("{}[__k] = __v;", param_name);
                    cpp("}}");
                }
                else
                {
                    // Primitives and std::string
                    cpp("{} = __response.{}();", param_name, param_name);
                }
            }
            else if (is_enum_type(lib, param_type))
            {
                // Enum types - cast from protobuf enum to IDL enum (drop reference for casting)
                std::string norm_type = normalize_type(param_type);
                cpp("{} = static_cast<{}>(__response.{}());", param_name, norm_type, param_name);
            }
            else
            {
                // Complex IDL-defined types (structs, vectors of structs, maps, etc.)
                // Use the struct's protobuf_deserialise method
                cpp("// Deserialize complex output parameter");
                cpp("{{");
                cpp("std::vector<char> param_buffer;");
                cpp("const auto& proto_param = __response.{}();", param_name);
                cpp("param_buffer.resize(proto_param.ByteSizeLong());");
                cpp("(void)proto_param.SerializeToArray(param_buffer.data(), static_cast<int>(param_buffer.size()));");
                cpp("{}.protobuf_deserialise(param_buffer);", param_name);
                cpp("}}");
            }
        }

        cpp("return __response.result();");
        cpp("}}");
        cpp("");
    }

    // Helper to write protobuf stub deserializer (request parsing) method
    void write_stub_protobuf_deserializer(const class_entity& lib,
        const class_entity& interface_entity,
        const std::shared_ptr<function_entity>& function,
        const std::string& interface_name,
        const std::string& package_name,
        writer& cpp)
    {
        (void)lib; // used for enum checking
        std::string function_name = function->get_name();

        // Collect input parameters
        std::vector<std::pair<std::string, std::string>> in_params;
        for (auto& param : function->get_parameters())
        {
            // Parameters are [in] by default unless marked [out]
            if (is_in_param(param) || (!is_in_param(param) && !is_out_param(param)))
            {
                std::string param_name = param.get_name();
                std::string param_type = param.get_type();
                in_params.emplace_back(param_name, param_type);
            }
        }

        // Suppress unused parameter warning
        (void)interface_entity;

        // Build stub deserializer signature with input parameters as non-const references
        cpp("template<>");
        std::string signature
            = "int " + interface_name + "::stub_deserialiser<rpc::serialiser::protocol_buffers>::" + function_name + "(";

        bool first_param = true;
        for (const auto& [param_name, param_type] : in_params)
        {
            if (!first_param)
                signature += ", ";
            first_param = false;

            // Transform input parameter type to match stub_deserialiser signature (non-const references)
            std::string final_param_type;
            if (param_type.find("rpc::shared_ptr") != std::string::npos
                || param_type.find("rpc::optimistic_ptr") != std::string::npos)
            {
                final_param_type = "rpc::interface_descriptor&";
            }
            else if (param_type.find('*') != std::string::npos)
            {
                final_param_type = "uint64_t&";
            }
            else if (param_type.find("&&") != std::string::npos)
            {
                // Rvalue references become lvalue references in stub_deserialiser
                std::string base_type = param_type.substr(0, param_type.find("&&"));
                // Remove any trailing whitespace
                while (!base_type.empty() && base_type.back() == ' ')
                {
                    base_type.pop_back();
                }
                final_param_type = base_type + "&";
            }
            else if (param_type.find('&') != std::string::npos)
            {
                // Remove const if present, keep reference
                std::string clean_type = param_type;
                size_t const_pos = clean_type.find("const ");
                if (const_pos != std::string::npos)
                {
                    clean_type.erase(const_pos, 6); // Remove "const "
                }
                final_param_type = clean_type;
            }
            else
            {
                // Plain type becomes non-const reference
                final_param_type = param_type + "&";
            }

            signature += final_param_type + " " + param_name;
        }

        if (!first_param)
            signature += ", ";
        signature += "const rpc::byte_span& __rpc_data)";
        cpp(signature);
        cpp("{{");

        // Deserialize protobuf request
        std::string request_message = interface_name + "_" + function_name + "Request";
        if (!package_name.empty())
            cpp("protobuf::{}::{} __request;", package_name, request_message);
        else
            cpp("protobuf::{} __request;", request_message);

        cpp("if (!__request.ParseFromArray(__rpc_data.data(), static_cast<int>(__rpc_data.size())))");
        cpp("{{");
        cpp("return rpc::error::STUB_DESERIALISATION_ERROR();");
        cpp("}}");

        // Only add blank line if we have input parameters
        if (!in_params.empty())
            cpp("");

        // Extract input parameters from request
        for (const auto& [param_name, param_type] : in_params)
        {
            // Check if this is a pointer type (IDL pointers become uint64_t in signatures - marshal address only)
            bool is_pointer = (param_type.find('*') != std::string::npos);
            // Check if this is an rpc::shared_ptr or rpc::optimistic_ptr (becomes interface_descriptor)
            bool is_interface = (param_type.find("rpc::shared_ptr") != std::string::npos
                                 || param_type.find("rpc::optimistic_ptr") != std::string::npos
                                 || param_type.find("rpc::interface_descriptor") != std::string::npos);

            if (is_interface)
            {
                // Interface types need special handling - deserialize proto message to interface_descriptor
                cpp("const auto& proto_{} = __request.{}();", param_name, param_name);
                cpp("{{");
                cpp("std::vector<char> __dz_buf(proto_{}.destination_zone_id().ByteSizeLong());", param_name);
                cpp("if (!proto_{}.destination_zone_id().SerializeToArray(__dz_buf.data(), "
                    "static_cast<int>(__dz_buf.size())))",
                    param_name);
                cpp("throw std::runtime_error(\"Failed to serialize nested destination_zone\");");
                cpp("{}.destination_zone_id.protobuf_deserialise(__dz_buf);", param_name);
                cpp("}}");
            }
            else if (is_pointer)
            {
                // Pointer types marshal the address only (uint64_t)
                cpp("{} = __request.{}();", param_name, param_name);
            }
            else if (is_simple_protobuf_type(param_type))
            {
                // Simple protobuf types (primitives, std::string, containers with scalar elements)
                std::string norm_type = normalize_type(param_type);
                if (norm_type == "std::vector<uint8_t>" || norm_type == "std::vector<unsignedchar>")
                {
                    // Use helper for bytes
                    cpp("rpc::serialization::protobuf::deserialize_bytes(__request.{}(), {});", param_name, param_name);
                }
                else if (is_scalar_vector_type(param_type))
                {
                    // Vector of scalar types - copy from repeated field
                    cpp("{}.clear();", param_name);
                    cpp("for (int __i = 0; __i < __request.{}_size(); ++__i) {{", param_name);
                    cpp("{}.push_back(__request.{}(__i));", param_name, param_name);
                    cpp("}}");
                }
                else if (is_scalar_map_type(param_type))
                {
                    // Map with scalar key/value - copy from protobuf map
                    cpp("{}.clear();", param_name);
                    cpp("for (const auto& [__k, __v] : __request.{}()) {{", param_name);
                    cpp("{}[__k] = __v;", param_name);
                    cpp("}}");
                }
                else
                {
                    // Primitives and std::string
                    cpp("{} = __request.{}();", param_name, param_name);
                }
            }
            else if (is_enum_type(lib, param_type))
            {
                // Enum types - cast from protobuf enum to IDL enum (drop reference for casting)
                std::string norm_type = normalize_type(param_type);
                cpp("{} = static_cast<{}>(__request.{}());", param_name, norm_type, param_name);
            }
            else
            {
                // Complex IDL-defined types (structs, vectors of structs, maps, etc.)
                // Use the struct's protobuf_deserialise method
                cpp("// Deserialize complex input parameter");
                cpp("{{");
                cpp("std::vector<char> param_buffer;");
                cpp("const auto& proto_param = __request.{}();", param_name);
                cpp("param_buffer.resize(proto_param.ByteSizeLong());");
                cpp("(void)proto_param.SerializeToArray(param_buffer.data(), static_cast<int>(param_buffer.size()));");
                cpp("{}.protobuf_deserialise(param_buffer);", param_name);
                cpp("}}");
            }
        }

        cpp("return rpc::error::OK();");
        cpp("}}");
        cpp("");
    }

    // Helper to write protobuf stub serializer (response creation) method
    void write_stub_protobuf_serializer(const class_entity& lib,
        const class_entity& interface_entity,
        const std::shared_ptr<function_entity>& function,
        const std::string& interface_name,
        const std::string& package_name,
        writer& cpp)
    {
        (void)lib; // used for enum checking
        std::string function_name = function->get_name();

        // Collect output parameters
        std::vector<std::pair<std::string, std::string>> out_params;
        for (auto& param : function->get_parameters())
        {
            if (is_out_param(param))
            {
                std::string param_name = param.get_name();
                std::string param_type = param.get_type();
                out_params.emplace_back(param_name, param_type);
            }
        }

        // Suppress unused parameter warning
        (void)interface_entity;

        // Build stub serializer signature with output parameters
        cpp("template<>");
        std::string signature
            = "int " + interface_name + "::stub_serialiser<rpc::serialiser::protocol_buffers>::" + function_name + "(";

        bool first_param = true;
        for (const auto& [param_name, param_type] : out_params)
        {
            if (!first_param)
                signature += ", ";
            first_param = false;

            // Transform output parameter type for stub_serialiser
            std::string final_param_type;
            if (param_type.find("rpc::shared_ptr") != std::string::npos
                || param_type.find("rpc::optimistic_ptr") != std::string::npos)
            {
                // Interface types by reference in stub_serialiser to match header
                final_param_type = "rpc::interface_descriptor&";
            }
            else if (param_type.find('*') != std::string::npos)
            {
                // Pointers become uint64_t by value (not reference) in stub_serialiser
                final_param_type = "uint64_t";
            }
            else if (param_type.find("&&") != std::string::npos)
            {
                // Rvalue references become const lvalue references
                std::string base_type = param_type.substr(0, param_type.find("&&"));
                while (!base_type.empty() && base_type.back() == ' ')
                {
                    base_type.pop_back();
                }
                final_param_type = "const " + base_type + "&";
            }
            else if (param_type.find('&') != std::string::npos)
            {
                // Keep const for output parameters in stub_serialiser
                final_param_type = param_type;
                if (final_param_type.find("const ") == std::string::npos)
                {
                    final_param_type = "const " + final_param_type;
                }
            }
            else
            {
                // Plain type becomes const reference
                final_param_type = "const " + param_type + "&";
            }

            signature += final_param_type + " " + param_name;
        }

        if (!first_param)
            signature += ", ";
        signature += "std::vector<char>& __buffer)";
        cpp(signature);
        cpp("{{");

        // Serialize protobuf response
        std::string response_message = interface_name + "_" + function_name + "Response";
        if (!package_name.empty())
            cpp("protobuf::{}::{} __response;", package_name, response_message);
        else
            cpp("protobuf::{} __response;", response_message);

        // Set output parameters in __response
        for (const auto& [param_name, param_type] : out_params)
        {
            // Check if this is a pointer type (IDL pointers become uint64_t in signatures - marshal address only)
            bool is_pointer = (param_type.find('*') != std::string::npos);
            // Check if this is an rpc::shared_ptr or rpc::optimistic_ptr (becomes interface_descriptor)
            bool is_interface = (param_type.find("rpc::shared_ptr") != std::string::npos
                                 || param_type.find("rpc::optimistic_ptr") != std::string::npos
                                 || param_type.find("rpc::interface_descriptor") != std::string::npos);

            if (is_interface)
            {
                // Interface types need special handling - serialize interface_descriptor to proto message
                cpp("auto* proto_{} = __response.mutable_{}();", param_name, param_name);
                cpp("{{");
                cpp("std::vector<char> __dz_buf;");
                cpp("{}.destination_zone_id.protobuf_serialise(__dz_buf);", param_name);
                cpp("if (!proto_{}->mutable_destination_zone_id()->ParseFromArray(__dz_buf.data(), "
                    "static_cast<int>(__dz_buf.size())))",
                    param_name);
                cpp("throw std::runtime_error(\"Failed to parse nested destination_zone\");");
                cpp("}}");
            }
            else if (is_pointer)
            {
                // Pointer types marshal the address only (uint64_t)
                cpp("__response.set_{}({});", param_name, param_name);
            }
            else if (is_simple_protobuf_type(param_type))
            {
                // Simple protobuf types (primitives, std::string, containers with scalar elements)
                std::string norm_type = normalize_type(param_type);
                if (norm_type == "std::vector<uint8_t>" || norm_type == "std::vector<unsignedchar>")
                {
                    // Use helper for bytes
                    cpp("rpc::serialization::protobuf::serialize_bytes({}, *__response.mutable_{}());",
                        param_name,
                        param_name);
                }
                else if (is_scalar_vector_type(param_type))
                {
                    // Vector of scalar types - copy elements to repeated field
                    cpp("for (const auto& __elem : {}) {{", param_name);
                    cpp("__response.add_{}(__elem);", param_name);
                    cpp("}}");
                }
                else if (is_scalar_map_type(param_type))
                {
                    // Map with scalar key/value - copy to protobuf map
                    cpp("auto* __map = __response.mutable_{}();", param_name);
                    cpp("for (const auto& [__k, __v] : {}) {{", param_name);
                    cpp("(*__map)[__k] = __v;");
                    cpp("}}");
                }
                else
                {
                    // Primitives and std::string
                    cpp("__response.set_{}({});", param_name, param_name);
                }
            }
            else if (is_enum_type(lib, param_type))
            {
                // Enum types can be directly assigned - cast to protobuf enum type
                std::string norm_type = normalize_type(param_type);
                cpp("__response.set_{}(static_cast<protobuf::{}::{}>({}));", param_name, package_name, norm_type, param_name);
            }
            else
            {
                // Complex IDL-defined types (structs, vectors of structs, maps, etc.)
                // Use the struct's protobuf_serialise method
                cpp("// Serialize complex output parameter");
                cpp("{{");
                cpp("std::vector<char> param_buffer;");
                cpp("{}.protobuf_serialise(param_buffer);", param_name);
                cpp("auto* proto_param = __response.mutable_{}();", param_name);
                cpp("(void)proto_param->ParseFromArray(param_buffer.data(), static_cast<int>(param_buffer.size()));");
                cpp("}}");
            }
        }

        // Set result code (always success for now)
        cpp("__response.set_result(rpc::error::OK());");

        // Serialize to buffer
        cpp("__buffer.clear();");
        cpp("__buffer.resize(__response.ByteSizeLong());");
        cpp("if (!__response.SerializeToArray(__buffer.data(), static_cast<int>(__buffer.size())))");
        cpp("{{");
        cpp("return rpc::error::STUB_DESERIALISATION_ERROR();");
        cpp("}}");
        cpp("return rpc::error::OK();");
        cpp("}}");
        cpp("");
    }

    // Helper to write protobuf C++ implementation for an interface
    void write_interface_protobuf_cpp(
        const class_entity& lib, const class_entity& interface_entity, const std::string& package_name, writer& cpp)
    {
        std::string interface_name = interface_entity.get_name();

        for (auto& function : interface_entity.get_functions())
        {
            if (function->get_entity_type() == entity_type::FUNCTION_METHOD)
            {
                write_proxy_protobuf_method(lib, interface_entity, function, interface_name, package_name, cpp);
                write_proxy_protobuf_deserializer(lib, interface_entity, function, interface_name, package_name, cpp);
                write_stub_protobuf_deserializer(lib, interface_entity, function, interface_name, package_name, cpp);
                write_stub_protobuf_serializer(lib, interface_entity, function, interface_name, package_name, cpp);
            }
        }
    }

    // Helper to extract inner type from template type (e.g., "std::vector<int>" -> "int")
    std::string extract_template_inner_type(const std::string& type_str)
    {
        size_t start = type_str.find('<');
        if (start == std::string::npos)
            return "";

        size_t end = type_str.rfind('>');
        if (end == std::string::npos || end <= start)
            return "";

        std::string inner = type_str.substr(start + 1, end - start - 1);
        // Trim whitespace
        size_t first = inner.find_first_not_of(' ');
        size_t last = inner.find_last_not_of(' ');
        if (first == std::string::npos)
            return "";
        return inner.substr(first, last - first + 1);
    }

    // Helper to check if a type is a std::vector
    bool is_std_vector(const std::string& type_str)
    {
        return type_str.find("std::vector<") == 0;
    }

    // Helper to check if a type is a std::array
    bool is_std_array(const std::string& type_str)
    {
        return type_str.find("std::array<") == 0;
    }

    // Helper to check if a type is a 128-bit integer
    bool is_int128_type(const std::string& type_str)
    {
        return type_str == "__int128" || type_str == "unsigned __int128" || type_str == "int128_t"
               || type_str == "uint128_t";
    }

    // Helper to extract element type from std::array<T, N>
    std::string extract_array_inner_type(const std::string& type_str)
    {
        size_t start = type_str.find('<');
        if (start == std::string::npos)
            return "";
        std::string inner_content;
        if (extract_template_content(type_str, start, inner_content) == std::string::npos)
            return "";
        std::string element_type;
        std::string size_param;
        if (split_template_args(inner_content, element_type, size_param))
            return element_type;
        return "";
    }

    // Helper to check if a type is a std::map
    bool is_std_map(const std::string& type_str)
    {
        return type_str.find("std::map<") == 0;
    }

    // Helper to extract map key and value types
    std::pair<std::string, std::string> extract_map_types(const std::string& type_str)
    {
        size_t start = type_str.find('<');
        if (start == std::string::npos)
            return {"", ""};

        size_t end = type_str.rfind('>');
        if (end == std::string::npos || end <= start)
            return {"", ""};

        std::string inner = type_str.substr(start + 1, end - start - 1);

        // Find the comma separating key and value types
        // Need to handle nested templates correctly
        int depth = 0;
        size_t comma_pos = std::string::npos;
        for (size_t i = 0; i < inner.length(); ++i)
        {
            if (inner[i] == '<')
                depth++;
            else if (inner[i] == '>')
                depth--;
            else if (inner[i] == ',' && depth == 0)
            {
                comma_pos = i;
                break;
            }
        }

        if (comma_pos == std::string::npos)
            return {"", ""};

        std::string key_type = inner.substr(0, comma_pos);
        std::string value_type = inner.substr(comma_pos + 1);

        // Trim whitespace
        size_t first = key_type.find_first_not_of(' ');
        size_t last = key_type.find_last_not_of(' ');
        if (first != std::string::npos)
            key_type = key_type.substr(first, last - first + 1);

        first = value_type.find_first_not_of(' ');
        last = value_type.find_last_not_of(' ');
        if (first != std::string::npos)
            value_type = value_type.substr(first, last - first + 1);

        return {key_type, value_type};
    }

    // Forward declaration
    void generate_struct_to_proto_copy(const class_entity& root_entity,
        const class_entity* struct_entity,
        const std::string& cpp_var,
        const std::string& proto_var,
        writer& cpp,
        const std::string& indent);

    // Helper to find a struct entity by name in the class hierarchy
    const class_entity* find_struct_by_name(const class_entity& root, const std::string& name)
    {
        // Extract unqualified name for cross-namespace lookup (e.g. "rpc::remote_object" -> "remote_object")
        std::string unqualified = name;
        size_t ns_pos = name.rfind("::");
        if (ns_pos != std::string::npos)
            unqualified = name.substr(ns_pos + 2);

        // Check if this entity is the struct we're looking for
        if (root.get_entity_type() == entity_type::STRUCT && (root.get_name() == name || root.get_name() == unqualified))
        {
            return &root;
        }

        // Search in namespace members
        for (auto& elem : root.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->get_entity_type() == entity_type::NAMESPACE)
            {
                auto& ns_entity = static_cast<const class_entity&>(*elem);
                const class_entity* result = find_struct_by_name(ns_entity, name);
                if (result)
                    return result;
            }
            else if (elem->get_entity_type() == entity_type::STRUCT)
            {
                auto& struct_entity = static_cast<const class_entity&>(*elem);
                if (struct_entity.get_name() == name || struct_entity.get_name() == unqualified)
                    return &struct_entity;
            }
        }

        return nullptr;
    }

    // Helper to generate code that copies fields from C++ struct to protobuf message
    void generate_struct_to_proto_copy(const class_entity& root_entity,
        const class_entity* struct_entity,
        const std::string& cpp_var,
        const std::string& proto_var,
        writer& cpp,
        const std::string& indent)
    {
        if (!struct_entity)
            return;

        // Copy each non-static field
        for (auto& member : struct_entity->get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members
                if (func_entity->is_static())
                    continue;

                std::string field_name = sanitize_field_name(func_entity->get_name());
                std::string field_type = func_entity->get_return_type();
                std::string member_name = func_entity->get_name();

                if (is_primitive_type(field_type) || field_type == "std::string")
                {
                    cpp("{}{}.set_{}({}.{});", indent, proto_var, field_name, cpp_var, member_name);
                }
                else
                {
                    // nested struct type - recursively copy fields
                    const class_entity* nested_struct = find_struct_by_name(root_entity, field_type);
                    if (nested_struct)
                    {
                        cpp("{}// Copy nested struct {} for field {}", indent, field_type, field_name);
                        generate_struct_to_proto_copy(root_entity,
                            nested_struct,
                            cpp_var + "." + member_name,
                            "(*" + proto_var + ".mutable_" + field_name + "())",
                            cpp,
                            indent);
                    }
                }
            }
        }
    }

    // Helper to generate code that copies fields from protobuf message to C++ struct
    void generate_proto_to_struct_copy(const class_entity& root_entity,
        const class_entity* struct_entity,
        const std::string& proto_var,
        const std::string& cpp_var,
        writer& cpp,
        const std::string& indent)
    {
        if (!struct_entity)
            return;

        // Copy each non-static field
        for (auto& member : struct_entity->get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members
                if (func_entity->is_static())
                    continue;

                std::string field_name = sanitize_field_name(func_entity->get_name());
                std::string field_type = func_entity->get_return_type();
                std::string member_name = func_entity->get_name();

                if (is_primitive_type(field_type) || field_type == "std::string")
                {
                    cpp("{}{}.{} = {}.{}();", indent, cpp_var, member_name, proto_var, field_name);
                }
                else
                {
                    // nested struct type - recursively copy fields
                    const class_entity* nested_struct = find_struct_by_name(root_entity, field_type);
                    if (nested_struct)
                    {
                        cpp("{}// Copy nested struct {} for field {}", indent, field_type, field_name);
                        generate_proto_to_struct_copy(root_entity,
                            nested_struct,
                            proto_var + "." + field_name + "()",
                            cpp_var + "." + member_name,
                            cpp,
                            indent);
                    }
                }
            }
        }
    }

    // Helper to get the full C++ namespace prefix for an entity using :: separator
    // e.g. a struct in secret_llama::v1_0 returns "::secret_llama::v1_0"
    std::string get_cpp_namespace_prefix(const class_entity& entity)
    {
        std::vector<std::string> parts;
        const class_entity* current = entity.get_owner();
        while (current != nullptr && !current->get_name().empty())
        {
            parts.push_back(current->get_name());
            current = current->get_owner();
        }
        std::string result;
        // NOLINTNEXTLINE(modernize-loop-convert): reverse_view is not available in this toolchain configuration.
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        {
            result += "::";
            result += *it;
        }
        return result;
    }

    // Helper to write protobuf struct member serialization implementations
    void write_struct_protobuf_cpp(
        const class_entity& root_entity, const class_entity& struct_entity, const std::string& package_name, writer& cpp)
    {
        std::string struct_name = struct_entity.get_name();
        std::string proto_message_name = sanitize_type_name(struct_name);

        // Compute the struct's C++ namespace prefix for qualifying type names (e.g. "::secret_llama::v1_0")
        std::string struct_cpp_ns = get_cpp_namespace_prefix(struct_entity);

        // Generate protobuf_serialise implementation
        cpp("void {}::protobuf_serialise(std::vector<char>& buffer) const", struct_name);
        cpp("{{");

        // Create protobuf message instance
        if (!package_name.empty())
            cpp("protobuf::{}::{} msg;", package_name, proto_message_name);
        else
            cpp("protobuf::{} msg;", proto_message_name);

        // Set fields from struct members
        for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members
                if (func_entity->is_static())
                    continue;

                std::string field_name = sanitize_field_name(func_entity->get_name());
                std::string field_type = func_entity->get_return_type();
                std::string member_name = func_entity->get_name();

                // Handle different type categories
                if (field_type == "unsigned __int128" || field_type == "__int128" || field_type == "uint128_t"
                    || field_type == "int128_t")
                {
                    // Serialize 128-bit integer into a uint128 message (lo = bits 0-63, hi = bits 64-127).
                    cpp("msg.mutable_{}()->set_lo(static_cast<uint64_t>({}));", field_name, member_name);
                    cpp("msg.mutable_{}()->set_hi(static_cast<uint64_t>({} >> 64));", field_name, member_name);
                }
                else if (is_primitive_type(field_type) || field_type == "std::string")
                {
                    // Simple primitive types
                    cpp("msg.set_{}({});", field_name, member_name);
                }
                else if (field_type == "std::vector<uint8_t>" || field_type == "std::vector<char>")
                {
                    // Special case: std::vector<uint8_t> and std::vector<char> map to protobuf bytes field
                    cpp("// Serialize {} as bytes", field_type);
                    cpp("rpc::serialization::protobuf::serialize_bytes({}, *msg.mutable_{}());", member_name, field_name);
                }
                else if (is_std_vector(field_type))
                {
                    // Handle std::vector<T>
                    std::string inner_type = extract_template_inner_type(field_type);

                    cpp("// Serialize std::vector<{}>", inner_type);
                    cpp("for (const auto& elem : {})", member_name);
                    cpp("{{");

                    if (is_int128_type(inner_type))
                    {
                        // Vector of 128-bit integers: add a uint128 message per element
                        cpp("auto* pb_elem = msg.add_{}();", field_name);
                        cpp("pb_elem->set_lo(static_cast<uint64_t>(elem));");
                        cpp("pb_elem->set_hi(static_cast<uint64_t>(elem >> 64));");
                    }
                    else if (is_primitive_type(inner_type) || inner_type == "std::string")
                    {
                        // Vector of primitives
                        cpp("msg.add_{}(elem);", field_name);
                    }
                    else
                    {
                        // Vector of structs - need to serialize each element
                        cpp("auto* proto_elem = msg.add_{}();", field_name);

                        // Look up the struct definition and generate field copying code
                        const class_entity* inner_struct = find_struct_by_name(root_entity, inner_type);
                        if (inner_struct)
                        {
                            generate_struct_to_proto_copy(
                                root_entity, inner_struct, "elem", "(*proto_elem)", cpp, "        ");
                        }
                        else if (inner_type.find("::") != std::string::npos && !is_enum_type(root_entity, inner_type))
                        {
                            // Cross-namespace struct: serialize via protobuf_serialise
                            cpp("// Serialize cross-namespace struct element {}", inner_type);
                            cpp("{{");
                            cpp("std::vector<char> elem_buf;");
                            cpp("elem.protobuf_serialise(elem_buf);");
                            cpp("if (!proto_elem->ParseFromArray(elem_buf.data(), static_cast<int>(elem_buf.size())))");
                            cpp("throw std::runtime_error(\"Failed to parse nested {}\");", inner_type);
                            cpp("}}");
                        }
                        else
                        {
                            cpp("// Warning: Could not find struct definition for {}", inner_type);
                        }
                    }

                    cpp("}}");
                }
                else if (is_std_array(field_type))
                {
                    // Handle std::array<T, N> — serialized as a repeated field (same as vector)
                    std::string inner_type = extract_array_inner_type(field_type);

                    cpp("// Serialize std::array<{}>", inner_type);
                    cpp("for (const auto& elem : {})", member_name);
                    cpp("{{");

                    if (is_int128_type(inner_type))
                    {
                        // Array of 128-bit integers: add a uint128 message per element
                        cpp("auto* pb_elem = msg.add_{}();", field_name);
                        cpp("pb_elem->set_lo(static_cast<uint64_t>(elem));");
                        cpp("pb_elem->set_hi(static_cast<uint64_t>(elem >> 64));");
                    }
                    else if (is_primitive_type(inner_type) || inner_type == "std::string")
                    {
                        cpp("msg.add_{}(elem);", field_name);
                    }
                    else
                    {
                        cpp("// TODO: Handle unsupported array element type {} for field {}", inner_type, field_name);
                    }

                    cpp("}}");
                }
                else if (is_std_map(field_type))
                {
                    // Handle std::map<K, V>
                    auto [key_type, value_type] = extract_map_types(field_type);

                    cpp("// Serialize std::map<{}, {}>", key_type, value_type);
                    cpp("for (const auto& [key, value] : {})", member_name);
                    cpp("{{");

                    if (is_primitive_type(value_type) || value_type == "std::string")
                    {
                        // Map with primitive values
                        cpp("(*msg.mutable_{}())[key] = value;", field_name);
                    }
                    else
                    {
                        // Map with struct values - need to serialize each value
                        cpp("auto& proto_value = (*msg.mutable_{}())[key];", field_name);

                        // Look up the struct definition and generate field copying code
                        const class_entity* value_struct = find_struct_by_name(root_entity, value_type);
                        if (value_struct)
                        {
                            generate_struct_to_proto_copy(
                                root_entity, value_struct, "value", "proto_value", cpp, "        ");
                        }
                        else
                        {
                            cpp("// Warning: Could not find struct definition for {}", value_type);
                        }
                    }

                    cpp("}}");
                }
                else
                {
                    // Check if it's a user-defined template instantiation (e.g. test_template<int>)
                    size_t tmpl_start = field_type.find('<');
                    bool is_user_template = tmpl_start != std::string::npos && field_type.find("std::") != 0
                                            && field_type.find("rpc::") != 0;

                    if (is_user_template)
                    {
                        cpp("// Serialize template instantiation {}", field_type);
                        cpp("{{");
                        cpp("std::vector<char> {}_buf;", field_name);
                        cpp("{}.protobuf_serialise({}_buf);", member_name, field_name);
                        cpp("if (!msg.mutable_{}()->ParseFromArray({}_buf.data(), static_cast<int>({}_buf.size())))",
                            field_name,
                            field_name,
                            field_name);
                        cpp("throw std::runtime_error(\"Failed to parse nested {} for field {}\");", field_type, field_name);
                        cpp("}}");
                    }
                    // check if it's a nested struct type
                    else if (const class_entity* nested_struct = find_struct_by_name(root_entity, field_type); nested_struct)
                    {
                        cpp("// Serialize nested struct {}", field_type);
                        cpp("{{");
                        cpp("std::vector<char> {}_buf;", field_name);
                        cpp("{}.protobuf_serialise({}_buf);", member_name, field_name);
                        cpp("if (!msg.mutable_{}()->ParseFromArray({}_buf.data(), static_cast<int>({}_buf.size())))",
                            field_name,
                            field_name,
                            field_name);
                        cpp("throw std::runtime_error(\"Failed to parse nested {} for field {}\");", field_type, field_name);
                        cpp("}}");
                    }
                    else if (is_enum_type(root_entity, field_type))
                    {
                        // Enum field: cast to proto enum via int
                        // Build fully-qualified proto type name
                        std::string proto_enum_type;
                        if (field_type.find("::") != std::string::npos)
                        {
                            // Qualified type like rpc::encoding -> ::protobuf::rpc::encoding
                            proto_enum_type = "::protobuf::" + field_type;
                        }
                        else if (!package_name.empty())
                        {
                            // Local enum in current package -> ::protobuf::<package>::<enum>
                            proto_enum_type = "::protobuf::" + package_name + "::" + field_type;
                        }
                        else
                        {
                            proto_enum_type = "::protobuf::" + field_type;
                        }
                        cpp("msg.set_{}(static_cast<{}>(static_cast<int>({})));", field_name, proto_enum_type, member_name);
                    }
                    else
                    {
                        cpp("// TODO: Handle unsupported type {} for field {}", field_type, field_name);
                        cpp("(void){};  // Suppress unused warning", member_name);
                    }
                }
            }
        }

        // Serialize to buffer
        cpp("buffer.clear();");
        cpp("buffer.resize(msg.ByteSizeLong());");
        cpp("if (!msg.SerializeToArray(buffer.data(), static_cast<int>(buffer.size())))");
        cpp("{{");
        cpp("throw std::runtime_error(\"Failed to serialize {} to protobuf\");", struct_name);
        cpp("}}");
        cpp("}}");
        cpp("");

        // Generate protobuf_deserialise implementation
        cpp("void {}::protobuf_deserialise(const std::vector<char>& buffer)", struct_name);
        cpp("{{");

        // Create protobuf message instance
        if (!package_name.empty())
            cpp("protobuf::{}::{} msg;", package_name, proto_message_name);
        else
            cpp("protobuf::{} msg;", proto_message_name);

        // Parse from buffer
        cpp("if (!msg.ParseFromArray(buffer.data(), static_cast<int>(buffer.size())))");
        cpp("{{");
        cpp("throw std::runtime_error(\"Failed to deserialize {} from protobuf\");", struct_name);
        cpp("}}");
        cpp("");

        // Extract fields to struct members
        for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members
                if (func_entity->is_static())
                    continue;

                std::string field_name = sanitize_field_name(func_entity->get_name());
                std::string field_type = func_entity->get_return_type();
                std::string member_name = func_entity->get_name();

                // Handle different type categories
                if (field_type == "unsigned __int128" || field_type == "__int128" || field_type == "uint128_t"
                    || field_type == "int128_t")
                {
                    // Deserialize 128-bit integer from a uint128 message (lo = bits 0-63, hi = bits 64-127).
                    cpp("{} = (static_cast<unsigned __int128>(msg.{}().hi()) << 64)"
                        " | static_cast<unsigned __int128>(msg.{}().lo());",
                        member_name,
                        field_name,
                        field_name);
                }
                else if (is_primitive_type(field_type) || field_type == "std::string")
                {
                    // Simple primitive types
                    cpp("{} = msg.{}();", member_name, field_name);
                }
                else if (field_type == "std::vector<uint8_t>" || field_type == "std::vector<char>")
                {
                    // Special case: std::vector<uint8_t> and std::vector<char> map to protobuf bytes field
                    cpp("// Deserialize {} from bytes", field_type);
                    cpp("rpc::serialization::protobuf::deserialize_bytes(msg.{}(), {});", field_name, member_name);
                }
                else if (is_std_vector(field_type))
                {
                    // Handle std::vector<T>
                    std::string inner_type = extract_template_inner_type(field_type);

                    cpp("// Deserialize std::vector<{}>", inner_type);
                    cpp("{}.clear();", member_name);
                    cpp("{}.reserve(msg.{}_size());", member_name, field_name);
                    cpp("for (int i = 0; i < msg.{}_size(); ++i)", field_name);
                    cpp("{{");

                    if (is_int128_type(inner_type))
                    {
                        // Vector of 128-bit integers: reconstruct from uint128 lo/hi fields
                        cpp("const auto& pb_elem = msg.{}(i);", field_name);
                        cpp("{}.push_back((static_cast<unsigned __int128>(pb_elem.hi()) << 64)"
                            " | static_cast<unsigned __int128>(pb_elem.lo()));",
                            member_name);
                    }
                    else if (is_primitive_type(inner_type) || inner_type == "std::string")
                    {
                        // Vector of primitives
                        cpp("{}.push_back(msg.{}(i));", member_name, field_name);
                    }
                    else
                    {
                        // Vector of structs - need to deserialize each element
                        cpp("const auto& proto_elem = msg.{}(i);", field_name);
                        cpp("{} elem;", inner_type);

                        // Look up the struct definition and generate field copying code
                        const class_entity* inner_struct = find_struct_by_name(root_entity, inner_type);
                        if (inner_struct)
                        {
                            generate_proto_to_struct_copy(root_entity, inner_struct, "proto_elem", "elem", cpp, "        ");
                        }
                        else if (inner_type.find("::") != std::string::npos && !is_enum_type(root_entity, inner_type))
                        {
                            // Cross-namespace struct: deserialize via protobuf_deserialise
                            cpp("// Deserialize cross-namespace struct element {}", inner_type);
                            cpp("{{");
                            cpp("std::vector<char> elem_buf(proto_elem.ByteSizeLong());");
                            cpp("if (!proto_elem.SerializeToArray(elem_buf.data(), "
                                "static_cast<int>(elem_buf.size())))");
                            cpp("throw std::runtime_error(\"Failed to serialize nested {}\");", inner_type);
                            cpp("elem.protobuf_deserialise(elem_buf);");
                            cpp("}}");
                        }
                        else
                        {
                            cpp("// Warning: Could not find struct definition for {}", inner_type);
                        }

                        cpp("{}.push_back(elem);", member_name);
                    }

                    cpp("}}");
                }
                else if (is_std_array(field_type))
                {
                    // Handle std::array<T, N> — deserialized from a repeated field
                    std::string inner_type = extract_array_inner_type(field_type);

                    cpp("// Deserialize std::array<{}>", inner_type);
                    cpp("for (size_t i = 0; i < {}.size() && i < static_cast<size_t>(msg.{}_size()); ++i)",
                        member_name,
                        field_name);
                    cpp("{{");

                    if (is_int128_type(inner_type))
                    {
                        cpp("const auto& pb_elem = msg.{}(static_cast<int>(i));", field_name);
                        cpp("{}[i] = (static_cast<unsigned __int128>(pb_elem.hi()) << 64)"
                            " | static_cast<unsigned __int128>(pb_elem.lo());",
                            member_name);
                    }
                    else if (is_primitive_type(inner_type) || inner_type == "std::string")
                    {
                        cpp("{}[i] = msg.{}(static_cast<int>(i));", member_name, field_name);
                    }
                    else
                    {
                        cpp("// TODO: Handle unsupported array element type {}", inner_type);
                    }

                    cpp("}}");
                }
                else if (is_std_map(field_type))
                {
                    // Handle std::map<K, V>
                    auto [key_type, value_type] = extract_map_types(field_type);

                    cpp("// Deserialize std::map<{}, {}>", key_type, value_type);
                    cpp("{}.clear();", member_name);
                    cpp("for (const auto& [key, proto_value] : msg.{}())", field_name);
                    cpp("{{");

                    if (is_primitive_type(value_type) || value_type == "std::string")
                    {
                        // Map with primitive values
                        cpp("{}[key] = proto_value;", member_name);
                    }
                    else
                    {
                        // Map with struct values - need to deserialize each value
                        cpp("{} value;", value_type);

                        // Look up the struct definition and generate field copying code
                        const class_entity* value_struct = find_struct_by_name(root_entity, value_type);
                        if (value_struct)
                        {
                            generate_proto_to_struct_copy(
                                root_entity, value_struct, "proto_value", "value", cpp, "        ");
                        }
                        else
                        {
                            cpp("// Warning: Could not find struct definition for {}", value_type);
                        }

                        cpp("{}[key] = value;", member_name);
                    }

                    cpp("}}");
                }
                else
                {
                    // Check if it's a user-defined template instantiation (e.g. test_template<int>)
                    size_t tmpl_start = field_type.find('<');
                    bool is_user_template = tmpl_start != std::string::npos && field_type.find("std::") != 0
                                            && field_type.find("rpc::") != 0;

                    if (is_user_template)
                    {
                        cpp("// Deserialize template instantiation {}", field_type);
                        cpp("{{");
                        cpp("std::vector<char> {}_buf(msg.{}().ByteSizeLong());", field_name, field_name);
                        cpp("if (!msg.{}().SerializeToArray({}_buf.data(), static_cast<int>({}_buf.size())))",
                            field_name,
                            field_name,
                            field_name);
                        cpp("throw std::runtime_error(\"Failed to serialize nested {} for field {}\");",
                            field_type,
                            field_name);
                        cpp("{}.protobuf_deserialise({}_buf);", member_name, field_name);
                        cpp("}}");
                    }
                    // check if it's a nested struct type
                    else if (const class_entity* nested_struct = find_struct_by_name(root_entity, field_type); nested_struct)
                    {
                        cpp("// Deserialize nested struct {}", field_type);
                        cpp("{{");
                        cpp("std::vector<char> {}_buf(msg.{}().ByteSizeLong());", field_name, field_name);
                        cpp("if (!msg.{}().SerializeToArray({}_buf.data(), static_cast<int>({}_buf.size())))",
                            field_name,
                            field_name,
                            field_name);
                        cpp("throw std::runtime_error(\"Failed to serialize nested {} for field {}\");",
                            field_type,
                            field_name);
                        cpp("{}.protobuf_deserialise({}_buf);", member_name, field_name);
                        cpp("}}");
                    }
                    else if (is_enum_type(root_entity, field_type))
                    {
                        // Enum field: cast from proto enum via int using fully-qualified C++ type
                        // to avoid member-name shadowing the type name in the cast
                        std::string qualified_enum_type;
                        if (field_type.find("::") != std::string::npos)
                        {
                            // Already qualified (e.g. rpc::encoding)
                            qualified_enum_type = "::" + field_type;
                        }
                        else if (!struct_cpp_ns.empty())
                        {
                            // Local enum - qualify with struct's namespace
                            qualified_enum_type = struct_cpp_ns + "::" + field_type;
                        }
                        else
                        {
                            qualified_enum_type = field_type;
                        }
                        cpp("{} = static_cast<{}>(static_cast<int>(msg.{}()));", member_name, qualified_enum_type, field_name);
                    }
                    else
                    {
                        cpp("// TODO: Handle unsupported type {} for field {}", field_type, field_name);
                    }
                }
            }
        }

        cpp("}}");
        cpp("");
    }

    // Helper to write explicit template instantiation protobuf implementations
    void write_template_instantiation_protobuf_cpp(const class_entity& struct_entity,
        const std::string& template_param,
        const std::string& concrete_name,
        const std::string& package_name,
        writer& cpp)
    {
        std::string struct_name = struct_entity.get_name();

        // Generate template specialization for protobuf_serialise
        cpp("template<>");
        cpp("void {}<{}>::protobuf_serialise(std::vector<char>& buffer) const", struct_name, template_param);
        cpp("{{");

        // Create protobuf message instance
        if (!package_name.empty())
            cpp("protobuf::{}::{} msg;", package_name, concrete_name);
        else
            cpp("protobuf::{} msg;", concrete_name);

        // Set fields from struct members
        for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members
                if (func_entity->is_static())
                    continue;

                std::string field_name = sanitize_field_name(func_entity->get_name());
                std::string member_name = func_entity->get_name();

                // For template structs, we assume simple field assignment
                cpp("msg.set_{}({});", field_name, member_name);
            }
        }

        // Serialize to buffer
        cpp("buffer.clear();");
        cpp("buffer.resize(msg.ByteSizeLong());");
        cpp("if (!msg.SerializeToArray(buffer.data(), static_cast<int>(buffer.size())))");
        cpp("{{");
        cpp("throw std::runtime_error(\"Failed to serialize protobuf message\");");
        cpp("}}");
        cpp("}}");
        cpp("");

        // Generate template specialization for protobuf_deserialise
        cpp("template<>");
        cpp("void {}<{}>::protobuf_deserialise(const std::vector<char>& buffer)", struct_name, template_param);
        cpp("{{");

        // Create protobuf message instance
        if (!package_name.empty())
            cpp("protobuf::{}::{} msg;", package_name, concrete_name);
        else
            cpp("protobuf::{} msg;", concrete_name);

        // Parse from buffer
        cpp("if (!msg.ParseFromArray(buffer.data(), static_cast<int>(buffer.size())))");
        cpp("{{");
        cpp("throw std::runtime_error(\"Failed to deserialize protobuf message\");");
        cpp("}}");
        cpp("");

        // Extract fields to struct members
        for (auto& member : struct_entity.get_elements(entity_type::STRUCTURE_MEMBERS))
        {
            if (member->get_entity_type() == entity_type::FUNCTION_VARIABLE)
            {
                auto func_entity = std::static_pointer_cast<function_entity>(member);

                // Skip static members
                if (func_entity->is_static())
                    continue;

                std::string field_name = sanitize_field_name(func_entity->get_name());
                std::string member_name = func_entity->get_name();

                // For template structs, we assume simple field assignment
                cpp("{} = msg.{}();", member_name, field_name);
            }
        }

        cpp("}}");
        cpp("");
    }

    // Helper to write protobuf C++ for a namespace
    void write_namespace_protobuf_cpp(
        const class_entity& root_entity, const class_entity& lib, const std::string& package_name, writer& cpp)
    {
        // Generate explicit template specializations FIRST so they are defined before any struct
        // implementation that might call them, preventing "explicit specialization after instantiation" errors.
        std::set<TemplateInstantiation> template_instantiations;
        collect_template_instantiations(lib, template_instantiations);

        for (const auto& inst : template_instantiations)
        {
            // Find the template struct - ONLY in the current namespace (not nested)
            const class_entity* found_template = nullptr;
            for (auto& struct_elem : lib.get_elements(entity_type::STRUCT))
            {
                auto& struct_entity = static_cast<const class_entity&>(*struct_elem);
                if (struct_entity.get_is_template() && struct_entity.get_name() == inst.template_name)
                {
                    found_template = &struct_entity;
                    break;
                }
            }

            // Only generate if the template is defined in THIS namespace
            if (found_template)
            {
                // Compute the protobuf package name (uses underscores, includes inline namespaces)
                std::string protobuf_package_name = get_namespace_name(lib);
                write_template_instantiation_protobuf_cpp(
                    *found_template, inst.template_param, inst.concrete_name, protobuf_package_name, cpp);
            }
        }

        // First pass: process namespaces and structs
        for (auto& elem : lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->is_in_import())
                continue;

            if (elem->get_entity_type() == entity_type::NAMESPACE)
            {
                auto& ns_entity = static_cast<const class_entity&>(*elem);
                bool is_inline = elem->has_value("inline");

                // Open namespace block for nested namespace
                if (is_inline)
                    cpp("inline namespace {}", elem->get_name());
                else
                    cpp("namespace {}", elem->get_name());
                cpp("{{");

                // Build extended package name for nested namespace
                // Inline namespaces are not included in protobuf package names
                std::string nested_package_name = package_name;
                if (!is_inline)
                {
                    if (!nested_package_name.empty())
                        nested_package_name += "::";
                    nested_package_name += elem->get_name();
                }

                // Recursively process the nested namespace with extended package name
                write_namespace_protobuf_cpp(root_entity, ns_entity, nested_package_name, cpp);

                // Close namespace block
                cpp("}}");
            }
            else if (elem->get_entity_type() == entity_type::STRUCT)
            {
                auto& struct_entity = static_cast<const class_entity&>(*elem);
                // Skip template structs - they need template specialization handling
                if (!struct_entity.get_is_template())
                {
                    // Compute the protobuf package name (uses underscores, includes inline namespaces)
                    std::string protobuf_package_name = get_namespace_name(lib);
                    write_struct_protobuf_cpp(root_entity, struct_entity, protobuf_package_name, cpp);
                }
            }
        }

        // Second pass: process interfaces
        for (auto& elem : lib.get_elements(entity_type::NAMESPACE_MEMBERS))
        {
            if (elem->is_in_import())
                continue;

            if (elem->get_entity_type() == entity_type::INTERFACE)
            {
                auto& interface_entity = static_cast<const class_entity&>(*elem);
                // Compute the protobuf package name (uses underscores, includes inline namespaces)
                std::string protobuf_package_name = get_namespace_name(lib);
                write_interface_protobuf_cpp(lib, interface_entity, protobuf_package_name, cpp);
            }
        }
    }

    // entry point - generates protobuf C++ serialization implementation
    void write_cpp_files(const class_entity& lib,
        std::ostream& cpp_stream,
        const std::vector<std::string>& namespaces,
        const std::filesystem::path& header_filename,
        const std::filesystem::path& protobuf_include_path,
        const std::vector<std::string>& additional_stub_headers)
    {
        writer cpp(cpp_stream);

        // Add includes
        for (const auto& additional_header : additional_stub_headers)
        {
            cpp("#include <{}>", additional_header);
        }

        cpp("#include <google/protobuf/message.h>");
        cpp("#include <rpc/rpc.h>");
        cpp("#include <rpc/serialization/protobuf/protobuf.h>");
        cpp("#include \"{}\"", header_filename.string());
        cpp("#include \"{}\"", protobuf_include_path.string());

        // // Include generated protobuf headers - one for each .proto file
        // // Note: protoc mirrors the source directory structure in the output directory
        // // e.g., "rpc/protobuf/schema/rpc.proto" -> "serialisers/rpc/protobuf/schema/rpc.pb.h"
        // // The build system adds serialisers/ to include path, so we include "rpc/protobuf/schema/rpc.pb.h"
        // for (const auto& proto_file : generated_proto_files)
        // {
        //     // Convert "rpc/protobuf/schema/rpc.proto" to "rpc/protobuf/schema/rpc.pb.h"
        //     // protoc preserves the directory structure relative to --proto_path
        //     std::string pb_header = proto_file.substr(0, proto_file.find_last_of('.')) + ".pb.h";
        //     cpp("#include \"{}\"", pb_header);
        // }
        cpp("");

        // Generate protobuf serialization methods
        // write_namespace_protobuf_cpp will recursively handle all namespace levels and open/close blocks as needed
        // Start with empty package_name - it will be built recursively as namespaces are traversed
        // Pass lib as both root_entity (for struct lookup) and current entity (for traversal)
        // Template instantiations are now generated inline within each namespace (between structs and interfaces)
        write_namespace_protobuf_cpp(lib, lib, "", cpp);

        // Suppress unused parameter warnings
        (void)namespaces;
    }
}
