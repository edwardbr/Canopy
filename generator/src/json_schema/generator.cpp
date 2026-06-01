/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "coreclasses.h" // Your parser API header
#include "cpp_parser.h"  // Your parser API header
#include "json_schema/generator.h"
#include "json_schema/writer.h"
#include "../../rpc/include/rpc/internal/build_modifiers.h"
#include "type_utils.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <algorithm>   // For std::find_if_not, std::reverse, std::find_if
#include <cctype>      // For std::isspace, std::iscntrl
#include <type_traits> // For underlying_type used with all_possible_members
#include <typeinfo>    // For dynamic_cast
#include <stdexcept>   // For std::stoll exceptions, std::stoi
#include <memory>      // For std::shared_ptr
#include <variant>     // For storing different definition info types

namespace json_schema
{

    // --- Forward Declarations ---
    struct SyntheticMethodInfo;
    using DefinitionInfoVariant = std::variant<const class_entity*, SyntheticMethodInfo>;
    using OrderedDefinitionItem = std::pair<std::string, DefinitionInfoVariant>;

    // The schema emitters below are a deep, mutually-recursive set of free
    // functions. Rather than thread a `schema_profile` through every signature
    // and recursive call, the active profile is published for the duration of a
    // single (single-threaded) generation via an RAII scope set at the public
    // entry points (write_json_schema_document / write_cpp_schema_accessors).
    // Any path reached without a scope falls back to the config-strict profile,
    // so output is unchanged unless a caller deliberately selects another.
    namespace
    {
        const schema_profile* g_active_profile = nullptr;

        const schema_profile& active_profile()
        {
            static const schema_profile fallback = config_strict_profile();
            return g_active_profile ? *g_active_profile : fallback;
        }

        struct active_profile_scope
        {
            const schema_profile* previous;
            explicit active_profile_scope(const schema_profile& profile)
                : previous(g_active_profile)
            {
                g_active_profile = &profile;
            }
            ~active_profile_scope() { g_active_profile = previous; }
            active_profile_scope(const active_profile_scope&) = delete;
            active_profile_scope& operator=(const active_profile_scope&) = delete;
        };
    } // namespace

    void map_idl_type_to_json_schema(
        const class_entity& root,
        const class_entity* current_context,
        const std::string& idl_type_name,
        const attributes& attribs,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map);
    void write_schema_definition(
        const class_entity& root,
        const class_entity& ent,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map);
    void write_synthetic_method_struct_definition(
        const class_entity& root,
        const SyntheticMethodInfo& info,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map);
    void find_definable_entities(
        const class_entity& current_entity,
        std::vector<OrderedDefinitionItem>& ordered_defs,
        bool include_imports = false);

    // --- Struct to hold info for synthetic method structs ---
    struct SyntheticMethodInfo
    {
        const class_entity* interface_entity = nullptr;
        const function_entity* method_entity = nullptr;
        bool is_send_struct = true;
    };

    bool parse_template_args(
        const std::string& type_name,
        std::string& container_name,
        std::vector<std::string>& args)
    {
        args.clear();
        size_t open_bracket = type_name.find('<');
        size_t close_bracket = type_name.rfind('>');
        if (open_bracket == std::string::npos || close_bracket == std::string::npos || close_bracket <= open_bracket)
            return false;
        container_name = generator::clean_type_name(type_name.substr(0, open_bracket));
        if (container_name.empty())
            return false;
        std::string args_str = type_name.substr(open_bracket + 1, close_bracket - open_bracket - 1);
        if (args_str.empty())
            return false;
        int bracket_level = 0;
        size_t current_arg_start = 0;
        for (size_t i = 0; i < args_str.length(); ++i)
        {
            if (args_str[i] == '<')
                bracket_level++;
            else if (args_str[i] == '>')
            {
                if (bracket_level > 0)
                    bracket_level--;
                else
                    return false;
            }
            else if (args_str[i] == ',' && bracket_level == 0)
            {
                std::string arg = generator::clean_type_name(args_str.substr(current_arg_start, i - current_arg_start));
                if (arg.empty())
                    return false;
                args.push_back(arg);
                current_arg_start = i + 1;
            }
        }
        if (bracket_level != 0)
            return false;
        std::string last_arg = generator::clean_type_name(args_str.substr(current_arg_start));
        if (last_arg.empty())
            return false;
        args.push_back(last_arg);
        return !args.empty();
    }

    static bool is_json_dom_schema_any_type(const std::string& type_name)
    {
        auto normalized = generator::clean_type_name(type_name);
        if (normalized.rfind("::", 0) == 0)
            normalized.erase(0, 2);

        return normalized == "json::v1::object" || normalized == "json::object";
    }

    static bool is_optional_schema_type(
        const std::string& type_name,
        std::string& inner_type)
    {
        std::string container_name;
        std::vector<std::string> args;
        if (!parse_template_args(type_name, container_name, args) || args.size() != 1)
            return false;

        if (container_name.rfind("::", 0) == 0)
            container_name.erase(0, 2);
        if (container_name != "rpc::optional")
            return false;

        inner_type = args.front();
        return !inner_type.empty();
    }

    static bool is_rpc_optional_schema_type(
        const std::string& type_name,
        std::string& inner_type)
    {
        std::string container_name;
        std::vector<std::string> args;
        if (!parse_template_args(type_name, container_name, args) || args.size() != 1)
            return false;

        if (container_name.rfind("::", 0) == 0)
            container_name.erase(0, 2);
        if (container_name != "rpc::optional")
            return false;

        inner_type = args.front();
        return !inner_type.empty();
    }

    static bool is_rpc_variant_schema_type(
        const std::string& type_name,
        std::vector<std::string>& alternative_types)
    {
        std::string container_name;
        std::vector<std::string> args;
        if (!parse_template_args(type_name, container_name, args))
            return false;

        if (container_name.rfind("::", 0) == 0)
            container_name.erase(0, 2);
        if (container_name != "rpc::variant" || args.empty())
            return false;

        alternative_types = std::move(args);
        return true;
    }

    // Strict JSON-number validator (no leading `+`, no bare `.5`, no trailing
    // `.`, exponent must have at least one digit). Mirrors the JSON grammar
    // so we never emit something like `+1` or `.5` into `default`.
    inline bool is_strict_json_number(const std::string& s)
    {
        if (s.empty())
            return false;
        size_t i = 0;
        if (s[i] == '-')
        {
            if (++i >= s.size())
                return false;
        }
        // Integer part: 0 or (1-9 followed by digits).
        if (s[i] == '0')
        {
            ++i;
        }
        else if (s[i] >= '1' && s[i] <= '9')
        {
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                ++i;
        }
        else
        {
            return false;
        }
        // Optional fraction.
        if (i < s.size() && s[i] == '.')
        {
            ++i;
            const size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                ++i;
            if (i == start)
                return false;
        }
        // Optional exponent.
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
        {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            const size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                ++i;
            if (i == start)
                return false;
        }
        return i == s.size();
    }

    // Strict JSON-string-literal validator. The IDL source's string literal
    // already uses C-style backslash escapes that overlap with JSON's, so
    // the common cases work, but `"abc" "def"` (adjacent C++ strings) or
    // unterminated content should be rejected.
    inline bool is_strict_json_string(const std::string& s)
    {
        if (s.size() < 2 || s.front() != '"' || s.back() != '"')
            return false;
        for (size_t i = 1; i + 1 < s.size(); ++i)
        {
            const char ch = s[i];
            // Embedded raw double quote terminates the string early — would
            // produce two-string adjacency like "abc" "def".
            if (ch == '"')
                return false;
            // Control characters must be escaped in JSON.
            if (static_cast<unsigned char>(ch) < 0x20)
                return false;
            if (ch == '\\')
            {
                if (i + 2 >= s.size())
                    return false;
                const char next = s[i + 1];
                if (next != '"' && next != '\\' && next != '/' && next != 'b' && next != 'f' && next != 'n'
                    && next != 'r' && next != 't' && next != 'u')
                    return false;
                if (next == 'u')
                {
                    if (i + 5 >= s.size())
                        return false;
                    for (size_t k = 2; k <= 5; ++k)
                    {
                        const char hx = s[i + k];
                        const bool hex = (hx >= '0' && hx <= '9') || (hx >= 'a' && hx <= 'f') || (hx >= 'A' && hx <= 'F');
                        if (!hex)
                            return false;
                    }
                    i += 4;
                }
                ++i; // step past the escaped character
            }
        }
        return true;
    }

    inline bool is_strict_json_literal(const std::string& s)
    {
        if (s == "true" || s == "false" || s == "null")
            return true;
        if (!s.empty() && s.front() == '"')
            return is_strict_json_string(s);
        return is_strict_json_number(s);
    }

    inline std::string unqualified_tail(const std::string& type_name)
    {
        auto cleaned = generator::clean_type_name(type_name);
        if (cleaned.rfind("::", 0) == 0)
            cleaned.erase(0, 2);
        const auto sep = cleaned.rfind("::");
        if (sep == std::string::npos)
            return cleaned;
        return cleaned.substr(sep + 2);
    }

    // Collect the unqualified names of every IDL enum present in the schema
    // generator's definition_info_map. Used by the default translator to
    // decide whether a scoped name like `A::B` should be rendered as a JSON
    // string (enum constant) or refused (potentially a scoped integer
    // constexpr that would conflict with the field's declared type).
    inline std::set<std::string> idl_enum_names_from_definitions(
        const std::map<std::string, DefinitionInfoVariant>& definition_info_map)
    {
        std::set<std::string> out;
        for (const auto& entry : definition_info_map)
        {
            if (!std::holds_alternative<const class_entity*>(entry.second))
                continue;
            const auto* cls = std::get<const class_entity*>(entry.second);
            if (!cls || cls->get_name().empty())
                continue;
            if (cls->get_entity_type() == entity_type::ENUM)
                out.insert(cls->get_name());
        }
        return out;
    }

    // Translate an IDL field default value (the verbatim text after `=` in
    // the IDL source) into a JSON literal that can be embedded in a schema's
    // `default` keyword. Returns empty when the default isn't representable
    // as a static JSON value — function calls, complex expressions, braced
    // initializers, integer constants disguised as enum-shaped names. The
    // caller then leaves `default` off the schema and the C++ struct /
    // converter still apply the original IDL expression.
    //
    // The field's declared type and the set of known IDL enum names are
    // consulted so that `A::B`-shaped defaults are only translated to a
    // string when the field is actually an enum-typed field. Scoped integer
    // constants such as `default_values::version_3` on a `uint8_t` field
    // are correctly rejected here.
    inline std::string translate_idl_default_to_json(
        std::string default_text,
        const std::string& field_cleaned_type,
        const std::set<std::string>& enum_definition_names)
    {
        const auto trim = [](std::string& s)
        {
            const auto first = s.find_first_not_of(" \t\n\r");
            if (first == std::string::npos)
            {
                s.clear();
                return;
            }
            const auto last = s.find_last_not_of(" \t\n\r");
            s = s.substr(first, last - first + 1);
        };

        trim(default_text);
        if (default_text.empty())
            return {};

        const auto accept = [](std::string candidate) -> std::string
        {
            return is_strict_json_literal(candidate) ? std::move(candidate) : std::string{};
        };

        if (default_text == "true" || default_text == "false")
            return default_text;
        if (default_text == "nullptr" || default_text == "NULL" || default_text == "std::nullopt")
            return "null";
        if (default_text == "{}" || default_text == "{ }")
            return {};

        // String literal: pass through if it parses as a strict JSON string.
        if (default_text.size() >= 2 && default_text.front() == '"' && default_text.back() == '"')
            return accept(std::move(default_text));

        // Scoped identifier (A::B::value). Only translate to a JSON string
        // when the field's type is a known IDL enum — otherwise the same
        // textual form could be a static integer constant
        // (e.g. `default_values::version_3` on a `uint8_t` field) and a
        // string literal would disagree with the field's declared type.
        if (default_text.find("::") != std::string::npos)
        {
            const auto sep = default_text.rfind("::");
            const auto tail = default_text.substr(sep + 2);
            if (tail.empty())
                return {};
            for (const char ch : tail)
            {
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
                    return {};
            }
            const auto field_unqualified = unqualified_tail(field_cleaned_type);
            if (!enum_definition_names.count(field_unqualified))
                return {};
            return accept(std::string("\"") + tail + "\"");
        }

        // Numeric literal. Tolerate the usual C++ suffixes (u/U/l/L/f/F and
        // combinations like "ull", "LL", "uLL"). Then validate the result
        // against the strict JSON number grammar so candidates like `+1`,
        // `.5`, `1.f`, or `1+2` are refused even though their characters
        // look numeric.
        const auto first = default_text.front();
        const bool starts_numeric = (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
        if (!starts_numeric)
            return {};

        std::string body = default_text;
        while (!body.empty()
               && (body.back() == 'u' || body.back() == 'U' || body.back() == 'l' || body.back() == 'L'
                   || body.back() == 'f' || body.back() == 'F'))
            body.pop_back();
        if (body.empty())
            return {};

        // Hex literals are valid C++ but not valid JSON; reject so the C++
        // path keeps applying them and the schema simply omits `default`.
        if (body.size() >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
            return {};

        return accept(std::move(body));
    }

    static void write_schema_metadata(
        const attributes& attribs,
        json_writer& writer)
    {
        std::string description = attribs.get_value("description");
        if (!description.empty())
            writer.write_string_property("description", description);
        if (attribs.has_value("deprecated"))
            writer.write_raw_property("deprecated", "true");
        // The IDL parser does not carry "default" through `attribs` natively;
        // it lives on function_entity. The struct property emitter pushes
        // the translated literal in as a synthetic attribute named "default"
        // before calling map_idl_type_to_json_schema, which then flows here.
        const auto translated_default = attribs.get_value("default");
        if (active_profile().include_defaults && !translated_default.empty())
            writer.write_raw_property("default", translated_default);
    }

    std::string variant_alternative_tag_for(
        std::string alternative_type,
        const taggable_idl_type_set& taggable_idl_types)
    {
        alternative_type = generator::clean_type_name(alternative_type);
        if (alternative_type.rfind("::", 0) == 0)
            alternative_type.erase(0, 2);

        // Runtime primitives: only these have rpc::variant_alternative_tag
        // specializations in <rpc/internal/variant.h>. Typedef aliases that
        // don't appear here (e.g. plain `int`, `error_code`) are intentionally
        // rejected — the runtime trait wouldn't compile for them.
        static const std::vector<std::pair<std::string, std::string>> primitive_tags = {
            {"bool", "bool"},
            {"std::int8_t", "int8"},
            {"int8_t", "int8"},
            {"std::int16_t", "int16"},
            {"int16_t", "int16"},
            {"std::int32_t", "int32"},
            {"int32_t", "int32"},
            {"std::int64_t", "int64"},
            {"int64_t", "int64"},
            {"std::uint8_t", "uint8"},
            {"uint8_t", "uint8"},
            {"std::uint16_t", "uint16"},
            {"uint16_t", "uint16"},
            {"std::uint32_t", "uint32"},
            {"uint32_t", "uint32"},
            {"std::uint64_t", "uint64"},
            {"uint64_t", "uint64"},
            {"float", "float"},
            {"double", "double"},
            {"std::string", "string"},
            {"string", "string"},
        };
        for (const auto& entry : primitive_tags)
        {
            if (alternative_type == entry.first)
                return entry.second;
        }

        // Template instantiations (std::vector<int> etc.) have no runtime
        // specialization. Reject so the enclosing variant is filtered out.
        if (alternative_type.find('<') != std::string::npos)
            return {};

        // json::v1::object is the raw JSON passthrough; it's intentionally
        // not a variant alternative target because its identity is "any JSON
        // value" and would not be distinguishable from sibling alternatives.
        if (alternative_type == "json::v1::object" || alternative_type == "json::object")
            return {};

        // IDL struct and enum types are tagged by their unqualified name —
        // but only if the IDL actually defines them. Typedefs, imported C++
        // helper types, and anything else will not have a corresponding
        // variant_alternative_tag specialization at runtime, so we reject
        // them here to keep schema and runtime in lockstep.
        std::string unqualified = alternative_type;
        const auto last = unqualified.rfind("::");
        if (last != std::string::npos)
            unqualified = unqualified.substr(last + 2);

        if (taggable_idl_types.count(unqualified))
            return unqualified;

        return {};
    }

    bool variant_alternatives_have_unique_tags(
        const std::vector<std::string>& alternative_types,
        const taggable_idl_type_set& taggable_idl_types,
        std::vector<std::string>& out_tags)
    {
        out_tags.clear();
        out_tags.reserve(alternative_types.size());
        std::set<std::string> seen;
        for (const auto& alt : alternative_types)
        {
            auto tag = variant_alternative_tag_for(alt, taggable_idl_types);
            if (tag.empty())
                return false;
            if (!seen.insert(tag).second)
                return false;
            out_tags.push_back(std::move(tag));
        }
        return true;
    }

    namespace
    {
        // Walk an IDL AST and collect the unqualified names of every
        // non-template struct and enum that the synchronous_generator will
        // emit a tag specialization for. Imported types are included since
        // their parent IDL emits the specialization in its own header.
        void collect_taggable_recurse(
            const class_entity& ent,
            taggable_idl_type_set& out)
        {
            for (const auto& cls : ent.get_classes())
            {
                if (!cls)
                    continue;
                const auto type = cls->get_entity_type();
                if (type == entity_type::NAMESPACE)
                {
                    collect_taggable_recurse(*cls, out);
                    continue;
                }
                if (type != entity_type::STRUCT && type != entity_type::ENUM)
                    continue;
                if (cls->get_is_template())
                    continue;
                if (cls->get_name().empty())
                    continue;
                out.insert(cls->get_name());
            }
        }
    }

    taggable_idl_type_set collect_taggable_idl_types(const class_entity& root_entity)
    {
        taggable_idl_type_set out;
        collect_taggable_recurse(root_entity, out);
        return out;
    }

    // Derive the runtime-supported taggable set directly from the schema
    // generator's definition_info_map so callers do not have to thread a
    // separate parameter.
    inline taggable_idl_type_set taggable_types_from_definitions(
        const std::map<std::string, DefinitionInfoVariant>& definition_info_map)
    {
        taggable_idl_type_set out;
        for (const auto& entry : definition_info_map)
        {
            if (!std::holds_alternative<const class_entity*>(entry.second))
                continue;
            const auto* cls = std::get<const class_entity*>(entry.second);
            if (!cls || cls->get_is_template() || cls->get_name().empty())
                continue;
            const auto type = cls->get_entity_type();
            if (type == entity_type::STRUCT || type == entity_type::ENUM)
                out.insert(cls->get_name());
        }
        return out;
    }

    void write_rpc_variant_json_schema(
        const class_entity& root,
        const class_entity* current_context,
        const std::vector<std::string>& alternative_types,
        const attributes& attribs,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        const auto taggable = taggable_types_from_definitions(definition_info_map);
        std::vector<std::string> tags;
        const bool tagged = variant_alternatives_have_unique_tags(alternative_types, taggable, tags);

        writer.open_object();
        write_schema_metadata(attribs, writer);

        if (!tagged)
        {
            // Fail-closed: emit a schema that rejects every value rather
            // than a permissive empty schema. Variants reaching this branch
            // contain alternatives the runtime cannot tag (template
            // instantiations, json::v1::object, typedef aliases) and the
            // converter generator filters their parent struct out, so no
            // serializer ever produces a value for this schema.
            writer.write_string_property(
                "description",
                "rpc::variant with non-taggable alternatives; no runtime support");
            writer.write_key("not");
            writer.open_object();
            writer.close_object();
            writer.close_object();
            return;
        }

        writer.write_key("oneOf");
        writer.open_array();
        for (size_t index = 0; index < alternative_types.size(); ++index)
        {
            writer.open_object();
            writer.write_string_property("type", "object");
            writer.write_key("properties");
            writer.open_object();
            writer.write_key(tags[index]);
            map_idl_type_to_json_schema(
                root,
                current_context,
                alternative_types[index],
                {},
                writer,
                definitions_needed,
                definitions_written,
                currently_processing,
                definition_info_map);
            writer.close_object();
            writer.write_key("required");
            writer.open_array();
            writer.write_array_string_element(tags[index]);
            writer.close_array();
            writer.write_raw_property("additionalProperties", "false");
            writer.close_object();
        }
        writer.close_array();
        writer.close_object();
    }

    std::string get_qualified_name(const entity& ent)
    {
        std::string qualified_name;
        std::vector<std::string> parts;
        const entity* current_entity_ptr = &ent;
        while (current_entity_ptr != nullptr && !current_entity_ptr->get_name().empty()
               && current_entity_ptr->get_name() != "__global__")
        {
            parts.push_back(current_entity_ptr->get_name());
            const auto* current_class_ptr = dynamic_cast<const class_entity*>(current_entity_ptr);
            if (current_class_ptr == nullptr)
                break;
            const class_entity* owner = current_class_ptr->get_owner();
            if (owner == nullptr || owner->get_name().empty() || owner->get_name() == "__global__"
                || (owner->get_entity_type() != entity_type::NAMESPACE && owner->get_entity_type() != entity_type::CLASS
                    && owner->get_entity_type() != entity_type::STRUCT
                    && owner->get_entity_type() != entity_type::INTERFACE))
            {
                break;
            }
            current_entity_ptr = owner;
        }
        std::reverse(parts.begin(), parts.end());
        for (size_t i = 0; i < parts.size(); ++i)
        {
            qualified_name += parts[i];
            if (i < parts.size() - 1)
                qualified_name += "_";
        }
        if (qualified_name.empty() && !ent.get_name().empty() && ent.get_name() != "__global__")
        {
            qualified_name = ent.get_name();
        }
        return qualified_name;
    }
    const class_entity* find_type_entity_upwards(
        const class_entity* start_scope,
        const std::string& type_name_cleaned)
    {
        const class_entity* current_scope = start_scope;
        while (current_scope != nullptr)
        {
            entity_type relevant_types = entity_type::NAMESPACE_MEMBERS | entity_type::STRUCTURE_MEMBERS;
            for (const auto& element_ptr : current_scope->get_elements(relevant_types))
            {
                if (!element_ptr)
                    continue;
                if (generator::clean_type_name(element_ptr->get_name()) == type_name_cleaned)
                {
                    entity_type et = element_ptr->get_entity_type();
                    if (et == entity_type::TYPEDEF || et == entity_type::STRUCT || et == entity_type::ENUM
                        || et == entity_type::CLASS)
                    {
                        const auto* found_direct_entity = dynamic_cast<const class_entity*>(element_ptr.get());
                        if (found_direct_entity)
                            return found_direct_entity;
                    }
                }
            }
            current_scope = current_scope->get_owner();
        }
        return nullptr;
    }

    bool try_parse_integer_literal(
        const std::string& expr,
        long long& value)
    {
        if (expr.empty())
            return false;

        std::string cleaned = generator::clean_type_name(expr);
        while (!cleaned.empty()
               && (cleaned.back() == 'u' || cleaned.back() == 'U' || cleaned.back() == 'l' || cleaned.back() == 'L'))
        {
            cleaned.pop_back();
        }

        if (cleaned.empty())
            return false;

        size_t start = (cleaned[0] == '+' || cleaned[0] == '-') ? 1u : 0u;
        if (start >= cleaned.size()
            || !std::all_of(
                cleaned.begin() + static_cast<std::ptrdiff_t>(start),
                cleaned.end(),
                [](unsigned char c) { return std::isdigit(c); }))
        {
            return false;
        }

        value = std::stoll(cleaned);
        return true;
    }

    const function_entity* find_constexpr_entity_upwards(
        const class_entity* start_scope,
        const std::string& name_cleaned)
    {
        const class_entity* current_scope = start_scope;
        while (current_scope != nullptr)
        {
            for (const auto& element_ptr : current_scope->get_elements(entity_type::CONSTEXPR))
            {
                if (!element_ptr || generator::clean_type_name(element_ptr->get_name()) != name_cleaned)
                    continue;

                if (auto constexpr_entity = dynamic_cast<const function_entity*>(element_ptr.get()); constexpr_entity)
                    return constexpr_entity;
            }
            current_scope = current_scope->get_owner();
        }
        return nullptr;
    }

    bool try_resolve_integer_constant(
        const class_entity* current_context,
        const std::string& expr,
        long long& value,
        int depth = 0)
    {
        if (depth > 16)
            return false;

        if (try_parse_integer_literal(expr, value))
            return true;

        if (current_context == nullptr)
            return false;

        auto expr_cleaned = generator::clean_type_name(expr);
        if (expr_cleaned.empty())
            return false;

        if (auto constexpr_entity = find_constexpr_entity_upwards(current_context, expr_cleaned); constexpr_entity)
        {
            auto default_value = generator::clean_type_name(constexpr_entity->get_default_value());
            if (default_value.empty() || default_value == expr_cleaned)
                return false;
            return try_resolve_integer_constant(current_context, default_value, value, depth + 1);
        }

        return false;
    }

    bool find_class_in_map(
        const std::string& type_name_cleaned,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map,
        const class_entity*& found_entity)
    {
        auto it = definition_info_map.find(type_name_cleaned);
        if (it != definition_info_map.end() && std::holds_alternative<const class_entity*>(it->second))
        {
            found_entity = std::get<const class_entity*>(it->second);
            return (found_entity != nullptr);
        }

        std::string qualified_schema_name = type_name_cleaned;
        while (qualified_schema_name.rfind("::", 0) == 0)
            qualified_schema_name.erase(0, 2);
        for (size_t pos = qualified_schema_name.find("::"); pos != std::string::npos;
            pos = qualified_schema_name.find("::", pos + 1))
        {
            qualified_schema_name.replace(pos, 2, "_");
        }
        it = definition_info_map.find(qualified_schema_name);
        if (it != definition_info_map.end() && std::holds_alternative<const class_entity*>(it->second))
        {
            found_entity = std::get<const class_entity*>(it->second);
            return (found_entity != nullptr);
        }

        size_t last_sep = type_name_cleaned.rfind('_');
        if (last_sep != std::string::npos)
        {
            std::string base_name = type_name_cleaned.substr(last_sep + 1);
            it = definition_info_map.find(base_name);
            if (it != definition_info_map.end() && std::holds_alternative<const class_entity*>(it->second))
            {
                found_entity = std::get<const class_entity*>(it->second);
                return (found_entity != nullptr);
            }
        }
        return false;
    }

    // Main function to write the definition for a specific NON-SYNTHETIC entity
    void write_schema_definition(
        const class_entity& root,
        const class_entity& ent,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        if (ent.get_is_template())
        {
            writer.open_object();
            writer.write_string_property("type", "null");
            writer.write_string_property("description", "Note: Schema generation skipped for template definition.");
            writer.close_object();
            return;
        }
        writer.open_object();
        std::string attr_description = ent.get_value("description");
        if (ent.has_value("deprecated"))
        {
            writer.write_raw_property("deprecated", "true");
        }
        switch (ent.get_entity_type())
        {
        case entity_type::STRUCT:
        case entity_type::CLASS:
        {
            if (!attr_description.empty())
            {
                writer.write_string_property("description", attr_description);
            }
            writer.write_string_property("type", "object");
            std::vector<std::string> required_fields;
            std::map<std::string, std::pair<std::string, attributes>> properties;
            const auto idl_enum_names = idl_enum_names_from_definitions(definition_info_map);
            for (const auto& element_ptr : ent.get_elements(entity_type::FUNCTION_VARIABLE))
            {
                if (!element_ptr || element_ptr->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;
                const auto* var = dynamic_cast<const function_entity*>(element_ptr.get());
                if (!var)
                    continue;
                // Static members are class-level constants, not per-instance
                // fields. The converter does not read/write them; the schema
                // should not require them either.
                if (var->is_static())
                    continue;
                std::string member_name = generator::clean_type_name(var->get_name());
                std::string raw_member_type = var->get_return_type();
                if (member_name.empty() || raw_member_type.empty())
                    continue;
                std::string cleaned_member_type = generator::clean_type_name(raw_member_type);
                if (cleaned_member_type.empty())
                    continue;
                // Translate the IDL `= default_value` text into a JSON
                // literal and stash it as a synthetic attribute on the field.
                // write_schema_metadata (and the inline metadata paths in
                // map_idl_type_to_json_schema) pick it up to emit `default`.
                attributes member_attribs = *var;
                if (auto translated_default = translate_idl_default_to_json(
                        var->get_default_value(), cleaned_member_type, idl_enum_names);
                    !translated_default.empty())
                {
                    member_attribs.push_back({"default", std::move(translated_default)});
                }
                properties[member_name] = {cleaned_member_type, std::move(member_attribs)};
                std::string optional_inner_type;
                if (!var->has_value("optional") && !is_optional_schema_type(cleaned_member_type, optional_inner_type))
                {
                    // A field with an IDL default is treated as not required
                    // by the schema — overlay can supply the value, and the
                    // converter falls back to the IDL expression when the
                    // user omits the key.
                    if (var->get_default_value().empty())
                        required_fields.push_back(member_name);
                }
            }
            if (!properties.empty())
            {
                writer.write_key("properties");
                writer.open_object();
                for (const auto& pair : properties)
                {
                    writer.write_key(pair.first);
                    map_idl_type_to_json_schema(
                        root,
                        &ent,
                        pair.second.first,
                        pair.second.second,
                        writer,
                        definitions_needed,
                        definitions_written,
                        currently_processing,
                        definition_info_map);
                }
                writer.close_object();
            }
            else
            {
                writer.write_key("properties");
                writer.open_object();
                writer.close_object();
            }
            if (active_profile().required == schema_profile::required_policy::idl_accurate
                && !required_fields.empty())
            {
                writer.write_key("required");
                writer.open_array();
                for (const auto& field : required_fields)
                {
                    writer.write_array_string_element(field);
                }
                writer.close_array();
            }
            if (active_profile().additional_properties_false)
                writer.write_raw_property("additionalProperties", "false");
            break;
        }
        case entity_type::ENUM:
        {
            // Collect name/value pairs once so we can emit both string-name
            // and integer alternatives; the converters write the canonical
            // string form, so the schema must accept it.
            struct enum_member
            {
                std::string name;
                int64_t value;
            };
            std::vector<enum_member> members;
            std::vector<std::string> value_descriptions;
            int64_t next_value = 0;
            using underlying = std::underlying_type<entity_type>::type;
            const auto all_possible_members = static_cast<entity_type>(
                static_cast<underlying>(entity_type::NAMESPACE_MEMBERS)
                | static_cast<underlying>(entity_type::STRUCTURE_MEMBERS));
            for (const auto& element_ptr : ent.get_elements(all_possible_members))
            {
                if (!element_ptr)
                    continue;
                std::string enum_value_name = generator::clean_type_name(element_ptr->get_name());
                if (!enum_value_name.empty() && enum_value_name.find_first_of("{}[]() \t\n\r") == std::string::npos)
                {
                    int64_t assigned_value = next_value;
                    const auto* value_entity = dynamic_cast<const function_entity*>(element_ptr.get());
                    if (value_entity)
                    {
                        std::string explicit_value_str = generator::clean_type_name(value_entity->get_return_type());
                        if (!explicit_value_str.empty())
                        {
                            try
                            {
                                assigned_value = std::stoll(explicit_value_str);
                            }
                            catch (const std::exception& e)
                            {
                                std::cerr << "exception has occurred explicit_value_str has value "
                                          << explicit_value_str << " resulting in this error: " << e.what() << "\n";
                            }
                        }
                    }
                    members.push_back({enum_value_name, assigned_value});
                    value_descriptions.push_back(enum_value_name + " = " + std::to_string(assigned_value));
                    next_value = assigned_value + 1;
                }
            }

            if (active_profile().enums == schema_profile::enum_form::string_only)
            {
                // String-name form only: tool consumers pick by name and the
                // integer alternative is noise. Emitted directly on the object
                // (not wrapped in oneOf) so the enum stays a simple string enum.
                writer.write_string_property("type", "string");
                writer.write_key("enum");
                writer.open_array();
                for (const auto& member : members)
                    writer.write_array_string_element(member.name);
                writer.close_array();
            }
            else
            {
                writer.write_key("oneOf");
                writer.open_array();

                // String-name form (canonical — what to_json_object writes).
                writer.open_object();
                writer.write_string_property("type", "string");
                writer.write_key("enum");
                writer.open_array();
                for (const auto& member : members)
                    writer.write_array_string_element(member.name);
                writer.close_array();
                writer.close_object();

                // Integer form (kept for legacy/compact JSON producers).
                writer.open_object();
                writer.write_string_property("type", "integer");
                writer.write_key("enum");
                writer.open_array();
                for (const auto& member : members)
                    writer.write_array_raw_element(std::to_string(member.value));
                writer.close_array();
                writer.close_object();

                writer.close_array();
            }

            if (!value_descriptions.empty())
            {
                std::string final_description = attr_description;
                if (!final_description.empty())
                {
                    final_description += ". ";
                }
                final_description += "Possible values: ";
                for (size_t i = 0; i < value_descriptions.size(); ++i)
                {
                    final_description += value_descriptions[i];
                    if (i < value_descriptions.size() - 1)
                    {
                        final_description += "; ";
                    }
                }
                writer.write_string_property("description", final_description);
            }
            else if (!attr_description.empty())
            {
                writer.write_string_property("description", attr_description);
            }
            break;
        }
        default:
        {
            if (!attr_description.empty())
            {
                writer.write_string_property("description", attr_description);
            }
            writer.write_string_property("type", "null");
            writer.write_string_property(
                "description",
                "Error: Unexpected entity type in write_schema_definition: "
                    + std::to_string(static_cast<int>(ent.get_entity_type())));
            break;
        }
        }
        writer.close_object();
    }

    // Writes definition for synthetic _send or _receive structs
    // ** RESTORED **
    void write_synthetic_method_struct_definition(
        const class_entity& root,
        const SyntheticMethodInfo& info,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        if (!info.interface_entity || !info.method_entity)
        {
            writer.open_object();
            writer.write_string_property("type", "null");
            writer.write_string_property("description", "Error: Invalid info for synthetic struct generation.");
            writer.close_object();
            return;
        }
        writer.open_object();
        writer.write_string_property("type", "object");
        std::string struct_type = info.is_send_struct ? "_send" : "_receive";
        std::string description = "Parameters for " + info.method_entity->get_name() + struct_type + " from interface "
                                  + info.interface_entity->get_name();
        writer.write_string_property("description", description);
        std::vector<std::string> required_fields;
        std::map<std::string, std::pair<std::string, attributes>> properties;
        for (const auto& param : info.method_entity->get_parameters())
        {
            bool is_in = param.has_value("in");
            bool is_out = param.has_value("out");
            bool implicitly_in = !is_in && !is_out;
            bool include_param = info.is_send_struct ? (is_in || implicitly_in) : is_out;
            if (include_param)
            {
                std::string param_name = generator::clean_type_name(param.get_name());
                std::string raw_param_type = param.get_type();
                if (param_name.empty() || raw_param_type.empty())
                    continue;
                std::string cleaned_param_type = generator::clean_type_name(raw_param_type);
                if (cleaned_param_type.empty())
                    continue;
                properties[param_name] = {cleaned_param_type, param};
                std::string optional_inner_type;
                if (!param.has_value("optional") && !is_optional_schema_type(cleaned_param_type, optional_inner_type))
                {
                    required_fields.push_back(param_name);
                }
            }
        }
        if (!properties.empty())
        {
            writer.write_key("properties");
            writer.open_object();
            for (const auto& pair : properties)
            {
                writer.write_key(pair.first);
                map_idl_type_to_json_schema(
                    root,
                    info.interface_entity,
                    pair.second.first,
                    pair.second.second,
                    writer,
                    definitions_needed,
                    definitions_written,
                    currently_processing,
                    definition_info_map);
            }
            writer.close_object();
        }
        else
        {
            writer.write_key("properties");
            writer.open_object();
            writer.close_object();
        }
        if (!required_fields.empty())
        {
            writer.write_key("required");
            writer.open_array();
            for (const auto& field : required_fields)
            {
                writer.write_array_string_element(field);
            }
            writer.close_array();
        }
        writer.write_raw_property("additionalProperties", "false");
        writer.close_object();
    }

    // Maps an IDL type name to its JSON schema representation
    // ** RESTORED **
    void map_idl_type_to_json_schema(
        const class_entity& root,
        const class_entity* current_context,
        const std::string& idl_type_name_in,
        const attributes& attribs,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        std::set<std::string>& definitions_written,
        const std::set<std::string>& currently_processing,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        std::string idl_type_name_cleaned = generator::clean_type_name(idl_type_name_in);
        if (idl_type_name_cleaned.empty())
        {
            writer.open_object();
            writer.write_string_property("type", "null");
            writer.write_string_property(
                "description", "Error: Invalid or empty type name encountered ('" + idl_type_name_in + "').");
            writer.close_object();
            return;
        }
        if (is_char_star(idl_type_name_cleaned) || idl_type_name_cleaned == "char*")
        {
            writer.open_object();
            std::string description = attribs.get_value("description");
            if (!description.empty())
                writer.write_string_property("description", description);

            if (attribs.has_value("deprecated"))
                writer.write_raw_property("deprecated", "true");
            writer.write_string_property("type", "string");
            writer.close_object();
            return;
        }
        std::string ignored_modifiers;
        generator::strip_reference_modifiers(idl_type_name_cleaned, ignored_modifiers);
        idl_type_name_cleaned = generator::unconst(idl_type_name_cleaned);

        if (is_json_dom_schema_any_type(idl_type_name_cleaned))
        {
            // json::v1::object is a passthrough for any JSON value (object,
            // array, string, number, boolean, or null). Emit a self-documenting
            // schema so the doc explains the intent; an explicit description on
            // the field still takes precedence.
            writer.open_object();
            if (attribs.get_value("description").empty())
                writer.write_string_property("description", "any JSON value (json::v1::object passthrough)");
            write_schema_metadata(attribs, writer);
            writer.close_object();
            return;
        }

        std::string optional_inner_type;
        if (is_rpc_optional_schema_type(idl_type_name_cleaned, optional_inner_type))
        {
            if (is_json_dom_schema_any_type(optional_inner_type))
            {
                writer.open_object();
                if (attribs.get_value("description").empty())
                    writer.write_string_property(
                        "description",
                        "optional, any JSON value (rpc::optional<json::v1::object> passthrough)");
                write_schema_metadata(attribs, writer);
                writer.close_object();
                return;
            }

            writer.open_object();
            write_schema_metadata(attribs, writer);
            writer.write_key("oneOf");
            writer.open_array();
            map_idl_type_to_json_schema(
                root,
                current_context,
                optional_inner_type,
                {},
                writer,
                definitions_needed,
                definitions_written,
                currently_processing,
                definition_info_map);
            writer.open_object();
            writer.write_string_property("type", "null");
            writer.close_object();
            writer.close_array();
            writer.close_object();
            return;
        }

        std::vector<std::string> variant_alternative_types;
        if (is_rpc_variant_schema_type(idl_type_name_cleaned, variant_alternative_types))
        {
            write_rpc_variant_json_schema(
                root,
                current_context,
                variant_alternative_types,
                attribs,
                writer,
                definitions_needed,
                definitions_written,
                currently_processing,
                definition_info_map);
            return;
        }

        std::string container_name;
        std::vector<std::string> template_args;
        if (parse_template_args(idl_type_name_cleaned, container_name, template_args))
        {
            if ((container_name == "std::vector" || container_name == "std::list" || container_name == "std::set"
                    || container_name == "std::unordered_set" || container_name == "std::deque"
                    || container_name == "std::queue" || container_name == "std::stack")
                && template_args.size() >= 1)
            {
                writer.open_object();
                writer.write_string_property("type", "array");
                std::string description = attribs.get_value("description");
                if (!description.empty())
                    writer.write_string_property("description", description);
                if (attribs.has_value("deprecated"))
                    writer.write_raw_property("deprecated", "true");
                // Set-like containers cannot carry duplicate values; surface
                // that contract in the schema so validators reject malformed
                // input rather than relying on the converter's silent dedupe.
                if (container_name == "std::set" || container_name == "std::unordered_set")
                {
                    writer.write_raw_property("uniqueItems", "true");
                }
                writer.write_key("items");
                map_idl_type_to_json_schema(
                    root,
                    current_context,
                    template_args[0],
                    {},
                    writer,
                    definitions_needed,
                    definitions_written,
                    currently_processing,
                    definition_info_map);
                writer.close_object();
                return;
            }
            else if (container_name == "std::array" && template_args.size() == 2)
            {
                writer.open_object();
                writer.write_string_property("type", "array");
                std::string description = attribs.get_value("description");
                if (!description.empty())
                    writer.write_string_property("description", description);
                if (attribs.has_value("deprecated"))
                    writer.write_raw_property("deprecated", "true");
                long long array_size = 0;
                if (try_resolve_integer_constant(current_context, template_args[1], array_size))
                {
                    if (array_size >= 0)
                    {
                        writer.write_raw_property("minItems", std::to_string(array_size));
                        writer.write_raw_property("maxItems", std::to_string(array_size));
                    }
                }
                else
                {
                    std::string current_desc = attribs.get_value("description");
                    std::string size_note = "[Note: Array size is non-literal: " + template_args[1] + "]";
                    writer.write_string_property(
                        "description", current_desc.empty() ? size_note : (current_desc + " " + size_note));
                }
                writer.write_key("items");
                map_idl_type_to_json_schema(
                    root,
                    current_context,
                    template_args[0],
                    {},
                    writer,
                    definitions_needed,
                    definitions_written,
                    currently_processing,
                    definition_info_map);
                writer.close_object();
                return;
            }
            else if ((container_name == "std::map" || container_name == "std::unordered_map") && template_args.size() == 2)
            {
                // String-keyed maps round-trip through a JSON object, matching
                // what to_json_object/from_json_object in convert.h emit and
                // accept. Non-string-keyed maps keep the legacy array-of-{k,v}
                // form (no JSON key can carry an arbitrary IDL type, and the
                // converter does not generate code for them).
                auto key_type = template_args[0];
                {
                    auto trimmed = generator::clean_type_name(key_type);
                    if (trimmed.rfind("::", 0) == 0)
                        trimmed.erase(0, 2);
                    key_type = trimmed;
                }
                const bool string_keyed = (key_type == "std::string" || key_type == "string");

                writer.open_object();
                std::string description = attribs.get_value("description");
                if (!description.empty())
                    writer.write_string_property("description", description);
                if (attribs.has_value("deprecated"))
                    writer.write_raw_property("deprecated", "true");

                if (string_keyed)
                {
                    writer.write_string_property("type", "object");
                    writer.write_key("additionalProperties");
                    map_idl_type_to_json_schema(
                        root,
                        current_context,
                        template_args[1],
                        {},
                        writer,
                        definitions_needed,
                        definitions_written,
                        currently_processing,
                        definition_info_map);
                }
                else
                {
                    writer.write_string_property("type", "array");
                    writer.write_key("items");
                    writer.open_object();
                    writer.write_string_property("type", "object");
                    writer.write_key("properties");
                    writer.open_object();
                    writer.write_key("k");
                    map_idl_type_to_json_schema(
                        root,
                        current_context,
                        template_args[0],
                        {},
                        writer,
                        definitions_needed,
                        definitions_written,
                        currently_processing,
                        definition_info_map);
                    writer.write_key("v");
                    map_idl_type_to_json_schema(
                        root,
                        current_context,
                        template_args[1],
                        {},
                        writer,
                        definitions_needed,
                        definitions_written,
                        currently_processing,
                        definition_info_map);
                    writer.close_object();
                    writer.write_key("required");
                    writer.open_array();
                    writer.write_array_string_element("k");
                    writer.write_array_string_element("v");
                    writer.close_array();
                    writer.write_raw_property("additionalProperties", "false");
                    writer.close_object();
                }
                writer.close_object();
                return;
            }
        }
        const class_entity* found_entity_ptr = nullptr;
        bool found = false;
        if (current_context != nullptr)
        {
            found_entity_ptr = find_type_entity_upwards(current_context, idl_type_name_cleaned);
            if (found_entity_ptr)
            {
                found = true;
            }
        }
        if (!found)
        {
            if (find_class_in_map(idl_type_name_cleaned, definition_info_map, found_entity_ptr))
            {
                found = true;
            }
        }
        if (found && found_entity_ptr != nullptr)
        {
            const class_entity& found_entity = *found_entity_ptr;
            entity_type et = found_entity.get_entity_type();
            if (et == entity_type::TYPEDEF)
            {
                std::string underlying_type = generator::clean_type_name(found_entity.get_alias_name());
                if (!underlying_type.empty())
                {
                    map_idl_type_to_json_schema(
                        root,
                        current_context,
                        underlying_type,
                        attribs,
                        writer,
                        definitions_needed,
                        definitions_written,
                        currently_processing,
                        definition_info_map);
                }
                else
                {
                    writer.open_object();
                    writer.write_string_property("type", "null");
                    writer.write_string_property("description", "Error: Typedef underlying type invalid.");
                    writer.close_object();
                }
                return;
            }
            else if (et == entity_type::STRUCT || et == entity_type::ENUM || et == entity_type::CLASS)
            {
                std::string qualified_name = get_qualified_name(found_entity);
                if (!qualified_name.empty())
                {
                    writer.open_object();
                    std::string description = attribs.get_value("description");
                    if (!description.empty())
                        writer.write_string_property("description", description);
                    if (attribs.has_value("deprecated"))
                        writer.write_raw_property("deprecated", "true");
                    const auto translated_default = attribs.get_value("default");
                    if (translated_default.empty() || !active_profile().include_defaults)
                    {
                        writer.write_string_property("$ref", "#/definitions/" + qualified_name);
                    }
                    else
                    {
                        // Draft-07 processors ignore siblings of `$ref`, so a
                        // bare `default` next to `$ref` would be invisible to
                        // external MCP/schema clients. Wrap the reference in
                        // `allOf` so `default` lives at the same level and is
                        // honoured. schema_default_values already walks
                        // `allOf` recursively.
                        writer.write_raw_property("default", translated_default);
                        writer.write_key("allOf");
                        writer.open_array();
                        writer.open_object();
                        writer.write_string_property("$ref", "#/definitions/" + qualified_name);
                        writer.close_object();
                        writer.close_array();
                    }
                    writer.close_object();
                    if (definitions_written.find(qualified_name) == definitions_written.end()
                        && currently_processing.find(qualified_name) == currently_processing.end())
                    {
                        definitions_needed.insert(qualified_name);
                    }
                }
                else
                {
                    writer.open_object();
                    writer.write_string_property("type", "null");
                    writer.write_string_property("description", "Error: Failed get qualified name for $ref.");
                    writer.close_object();
                }
                return;
            }
        }
        std::string idl_type_name = idl_type_name_cleaned;
        writer.open_object();
        std::string description = attribs.get_value("description");
        if (!description.empty())
            writer.write_string_property("description", description);
        if (attribs.has_value("deprecated"))
            writer.write_raw_property("deprecated", "true");
        {
            const auto translated_default = attribs.get_value("default");
            if (active_profile().include_defaults && !translated_default.empty())
                writer.write_raw_property("default", translated_default);
        }
        if (is_int8(idl_type_name) || is_uint8(idl_type_name) || is_int16(idl_type_name) || is_uint16(idl_type_name)
            || is_int32(idl_type_name) || is_uint32(idl_type_name) || is_int64(idl_type_name) || is_uint64(idl_type_name)
            || is_long(idl_type_name) || is_ulong(idl_type_name) || idl_type_name == "int" || idl_type_name == "char")
        {
            writer.write_string_property("type", "integer");
        }
        else if (is_float(idl_type_name) || is_double(idl_type_name))
        {
            writer.write_string_property("type", "number");
        }
        else if (is_bool(idl_type_name))
        {
            writer.write_string_property("type", "boolean");
        }
        else if (idl_type_name == "string" || idl_type_name == "std::string")
        {
            writer.write_string_property("type", "string");
            std::string format = attribs.get_value("format");
            if (!format.empty())
                writer.write_string_property("format", format);
        }
        else
        {
            writer.write_string_property("type", "null");
            std::string error_msg = "Error: Could not resolve IDL type '" + idl_type_name_in + "'";
            if (current_context != nullptr)
            {
                std::string context_name = get_qualified_name(*current_context);
                if (context_name.empty())
                    context_name = current_context->get_name();
                error_msg += " used within scope '" + context_name + "'";
            }
            error_msg += " (Searched scope and global definitions). Stripped type checked: '" + idl_type_name + "'.";
            writer.write_string_property("description", error_msg);
        }
        writer.close_object();
    }

    // Finds interfaces and adds synthetic struct info, populates ordered list
    // ** RESTORED **
    void find_definable_entities(
        const class_entity& current_entity,
        std::vector<OrderedDefinitionItem>& ordered_defs,
        bool include_imports)
    {
        entity_type et = current_entity.get_entity_type();
        if (current_entity.is_in_import() && !include_imports)
            return;
        bool is_template_definition = current_entity.get_is_template();
        std::string qualified_name = get_qualified_name(current_entity);
        if (!qualified_name.empty() && !is_template_definition)
        {
            if (et == entity_type::STRUCT || et == entity_type::ENUM || et == entity_type::CLASS)
            {
                bool found = false;
                for (const auto& item : ordered_defs)
                {
                    if (item.first == qualified_name)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    ordered_defs.push_back({qualified_name, &current_entity});
                }
            }
            else if (et == entity_type::INTERFACE)
            {
                for (const auto& element_ptr : current_entity.get_elements(entity_type::FUNCTION_METHOD))
                {
                    if (!element_ptr || element_ptr->get_entity_type() != entity_type::FUNCTION_METHOD)
                        continue;
                    const auto* method = dynamic_cast<const function_entity*>(element_ptr.get());
                    if (!method)
                        continue;
                    std::string method_name = generator::clean_type_name(method->get_name());
                    if (method_name.empty())
                        continue;
                    std::string send_struct_name = qualified_name + "_" + method_name + "_send";
                    std::string receive_struct_name = qualified_name + "_" + method_name + "_receive";
                    ordered_defs.push_back(
                        {send_struct_name,
                            SyntheticMethodInfo{FLD(interface_entity) & current_entity,
                                FLD(method_entity) method,
                                FLD(is_send_struct) true}});
                    ordered_defs.push_back(
                        {receive_struct_name,
                            SyntheticMethodInfo{FLD(interface_entity) & current_entity,
                                FLD(method_entity) method,
                                FLD(is_send_struct) false}});
                }
            }
        }
        entity_type members_to_get = entity_type::TYPE_NULL;
        if (!is_template_definition)
        {
            if (et == entity_type::NAMESPACE || current_entity.get_owner() == nullptr
                || current_entity.get_name() == "__global__")
            {
                members_to_get = entity_type::NAMESPACE_MEMBERS;
            }
            else if (et == entity_type::STRUCT || et == entity_type::CLASS || et == entity_type::INTERFACE)
            {
                members_to_get = entity_type::STRUCTURE_MEMBERS | entity_type::NAMESPACE_MEMBERS;
            }
        }
        if (members_to_get != entity_type::TYPE_NULL)
        {
            for (const auto& element_ptr : current_entity.get_elements(members_to_get))
            {
                if (!element_ptr || (element_ptr->is_in_import() && !include_imports))
                    continue;
                const auto* child_class = dynamic_cast<const class_entity*>(element_ptr.get());
                if (!child_class)
                    continue;
                find_definable_entities(*child_class, ordered_defs, include_imports);
            }
        }
    }

    std::string sanitise_cpp_identifier(std::string name)
    {
        for (auto& ch : name)
        {
            const auto value = static_cast<unsigned char>(ch);
            if (!std::isalnum(value) && ch != '_')
                ch = '_';
        }

        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
            name.insert(name.begin(), '_');

        return name;
    }

    void collect_definition_info(
        const class_entity& root_entity,
        std::vector<OrderedDefinitionItem>& ordered_defs,
        std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        find_definable_entities(root_entity, ordered_defs, false);
        for (const auto& item : ordered_defs)
        {
            definition_info_map[item.first] = item.second;
        }

        std::vector<OrderedDefinitionItem> visible_defs;
        find_definable_entities(root_entity, visible_defs, true);
        for (const auto& item : visible_defs)
        {
            if (definition_info_map.find(item.first) == definition_info_map.end())
                definition_info_map[item.first] = item.second;
        }
    }

    void write_definition_set(
        const class_entity& root_entity,
        json_writer& writer,
        std::set<std::string>& definitions_needed,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        std::set<std::string> definitions_written;
        size_t processed_count = 0;
        const size_t max_processed = (definition_info_map.size()) * 3 + 20;
        std::set<std::string> currently_processing;
        while (!definitions_needed.empty() && processed_count++ < max_processed)
        {
            std::string current_name = *definitions_needed.begin();
            definitions_needed.erase(definitions_needed.begin());
            if (definitions_written.count(current_name) || currently_processing.count(current_name))
                continue;
            auto it_info = definition_info_map.find(current_name);
            if (it_info != definition_info_map.end())
            {
                currently_processing.insert(current_name);
                writer.write_key(current_name);
                std::visit(
                    [&](auto&& arg)
                    {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr (std::is_same_v<T, const class_entity*>)
                        {
                            write_schema_definition(
                                root_entity,
                                *arg,
                                writer,
                                definitions_needed,
                                definitions_written,
                                currently_processing,
                                definition_info_map);
                        }
                        else if constexpr (std::is_same_v<T, SyntheticMethodInfo>)
                        {
                            write_synthetic_method_struct_definition(
                                root_entity,
                                arg,
                                writer,
                                definitions_needed,
                                definitions_written,
                                currently_processing,
                                definition_info_map);
                        }
                    },
                    it_info->second);
                currently_processing.erase(current_name);
                definitions_written.insert(current_name);
            }
            else
            {
                writer.write_key(current_name);
                writer.open_object();
                writer.write_string_property("type", "null");
                writer.write_string_property("description", "Error: Definition info not found for '" + current_name + "'.");
                writer.close_object();
                definitions_written.insert(current_name);
            }
        }
        if (processed_count >= max_processed && !definitions_needed.empty())
        {
            writer.write_key("__GENERATION_ERROR__");
            writer.open_object();
            writer.write_string_property("description", "Max processing limit reached.");
            writer.write_key("remaining_definitions");
            writer.open_array();
            for (const auto& remaining_name : definitions_needed)
            {
                writer.write_array_string_element(remaining_name);
            }
            writer.close_array();
            writer.close_object();
        }
    }

    void write_json_schema_document(
        const class_entity& root_entity,
        std::ostream& os,
        const std::string& schema_title,
        const std::string& root_definition_name,
        const schema_profile& profile)
    {
        active_profile_scope profile_guard(profile);
        json_writer writer(os);
        std::vector<OrderedDefinitionItem> ordered_defs;
        std::map<std::string, DefinitionInfoVariant> definition_info_map;
        collect_definition_info(root_entity, ordered_defs, definition_info_map);

        std::set<std::string> definitions_needed;
        if (root_definition_name.empty())
        {
            for (const auto& item : ordered_defs)
            {
                definitions_needed.insert(item.first);
            }
        }
        else
        {
            definitions_needed.insert(root_definition_name);
        }

        writer.open_object();
        writer.write_string_property("$schema", "http://json-schema.org/draft-07/schema#");
        // $id is config-profile-only and opt-in: emitted solely when a stable
        // id_path has been supplied, so existing callers stay byte-identical.
        if (profile.emit_id && !profile.id_path.empty())
            writer.write_string_property("$id", profile.id_base + profile.id_path);
        writer.write_string_property(
            "title", root_definition_name.empty() ? schema_title : schema_title + "::" + root_definition_name);
        if (!root_definition_name.empty())
            writer.write_string_property("$ref", "#/definitions/" + root_definition_name);
        writer.write_key("definitions");
        writer.open_object();
        write_definition_set(root_entity, writer, definitions_needed, definition_info_map);
        writer.close_object();
        writer.close_object();
        os << "\n";
    }

    std::vector<std::string> get_cpp_namespace_parts(const class_entity& ent)
    {
        std::vector<std::string> parts;
        const class_entity* owner = ent.get_owner();
        while (owner != nullptr && !owner->get_name().empty() && owner->get_name() != "__global__")
        {
            if (owner->get_entity_type() == entity_type::NAMESPACE)
                parts.push_back(owner->get_name());
            owner = owner->get_owner();
        }
        std::reverse(parts.begin(), parts.end());
        return parts;
    }

    std::string get_cpp_member_scope_name(const class_entity& ent)
    {
        std::vector<std::string> parts;
        const class_entity* current = &ent;
        while (current != nullptr && !current->get_name().empty() && current->get_name() != "__global__")
        {
            parts.push_back(current->get_name());
            const class_entity* owner = current->get_owner();
            if (owner == nullptr || owner->get_name().empty() || owner->get_name() == "__global__"
                || owner->get_entity_type() == entity_type::NAMESPACE)
            {
                break;
            }
            current = owner;
        }
        std::reverse(parts.begin(), parts.end());

        std::string name;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i != 0)
                name += "::";
            name += parts[i];
        }
        return name;
    }

    std::string get_cpp_fully_qualified_scope_name(const class_entity& ent)
    {
        auto namespaces = get_cpp_namespace_parts(ent);
        auto name = std::string("::");
        for (const auto& ns : namespaces)
        {
            name += ns;
            name += "::";
        }
        name += get_cpp_member_scope_name(ent);
        return name;
    }

    const class_entity* get_struct_accessor_entity(
        const std::string& definition_name,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        auto it = definition_info_map.find(definition_name);
        if (it == definition_info_map.end() || !std::holds_alternative<const class_entity*>(it->second))
            return nullptr;

        const auto* ent = std::get<const class_entity*>(it->second);
        if (!ent || ent->get_entity_type() != entity_type::STRUCT || ent->get_is_template())
            return nullptr;

        return ent;
    }

    std::string write_definition_body(
        const class_entity& root_entity,
        const std::string& definition_name,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map,
        std::set<std::string>* discovered_definitions = nullptr)
    {
        std::stringstream body_stream;
        json_writer writer(body_stream);
        std::set<std::string> definitions_needed;
        std::set<std::string> definitions_written;
        std::set<std::string> currently_processing;

        auto it_info = definition_info_map.find(definition_name);
        if (it_info != definition_info_map.end())
        {
            currently_processing.insert(definition_name);
            std::visit(
                [&](auto&& arg)
                {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, const class_entity*>)
                    {
                        write_schema_definition(
                            root_entity,
                            *arg,
                            writer,
                            definitions_needed,
                            definitions_written,
                            currently_processing,
                            definition_info_map);
                    }
                    else if constexpr (std::is_same_v<T, SyntheticMethodInfo>)
                    {
                        write_synthetic_method_struct_definition(
                            root_entity, arg, writer, definitions_needed, definitions_written, currently_processing, definition_info_map);
                    }
                },
                it_info->second);
        }
        else
        {
            writer.open_object();
            writer.write_string_property("type", "null");
            writer.write_string_property("description", "Error: Definition info not found for '" + definition_name + "'.");
            writer.close_object();
        }

        if (discovered_definitions)
        {
            discovered_definitions->insert(definitions_needed.begin(), definitions_needed.end());
        }

        return body_stream.str();
    }

    std::vector<std::string> collect_definition_closure(
        const class_entity& root_entity,
        const std::set<std::string>& root_definition_names,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        std::set<std::string> definitions_needed = root_definition_names;
        std::set<std::string> definitions_written;
        std::vector<std::string> ordered_names;
        size_t processed_count = 0;
        const size_t max_processed = (definition_info_map.size() + root_definition_names.size()) * 3 + 20;

        while (!definitions_needed.empty() && processed_count++ < max_processed)
        {
            auto current_name = *definitions_needed.begin();
            definitions_needed.erase(definitions_needed.begin());
            if (definitions_written.count(current_name))
                continue;

            ordered_names.push_back(current_name);
            definitions_written.insert(current_name);

            std::set<std::string> discovered_definitions;
            (void)write_definition_body(root_entity, current_name, definition_info_map, &discovered_definitions);
            for (const auto& discovered_definition : discovered_definitions)
            {
                if (!definitions_written.count(discovered_definition))
                    definitions_needed.insert(discovered_definition);
            }
        }

        if (processed_count >= max_processed && !definitions_needed.empty())
            ordered_names.push_back("__GENERATION_ERROR__");

        return ordered_names;
    }

    std::map<
        std::string,
        std::string>
    collect_definition_bodies(
        const class_entity& root_entity,
        const std::vector<std::string>& definition_names,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        std::map<std::string, std::string> definition_bodies;
        for (const auto& definition_name : definition_names)
        {
            if (definition_name == "__GENERATION_ERROR__")
            {
                std::stringstream body_stream;
                json_writer writer(body_stream);
                writer.open_object();
                writer.write_string_property("description", "Max processing limit reached.");
                writer.close_object();
                definition_bodies[definition_name] = body_stream.str();
                continue;
            }

            definition_bodies[definition_name] = write_definition_body(root_entity, definition_name, definition_info_map);
        }
        return definition_bodies;
    }

    std::string get_definition_inner_schema_expression(
        const std::string& definition_name,
        const std::string& detail_namespace,
        const std::map<
            std::string,
            DefinitionInfoVariant>& definition_info_map)
    {
        if (const auto* ent = get_struct_accessor_entity(definition_name, definition_info_map); ent)
            return get_cpp_fully_qualified_scope_name(*ent) + "::get_inner_schema()";

        return "::canopy::generated_schema::" + detail_namespace + "::" + sanitise_cpp_identifier(definition_name)
               + "_inner_schema()";
    }

    void write_schema_document_start(
        std::ostream& os,
        const std::string& schema_title,
        const std::string& root_definition_name,
        const schema_profile& profile)
    {
        os << "    std::string schema;\n";
        os << "    schema += R\"canopy_json(\n";
        os << "{\n\n";
        os << "  \"$schema\": \"http://json-schema.org/draft-07/schema#\",\n";
        // $id mirrors write_json_schema_document: opt-in, config-profile only, so
        // the embedded-string accessor stays byte-identical for existing callers
        // and matches the standalone .json document when an id_path is supplied.
        if (profile.emit_id && !profile.id_path.empty())
            os << "  \"$id\": \"" << profile.id_base << profile.id_path << "\",\n";
        os << "  \"title\": \""
           << (root_definition_name.empty() ? schema_title : schema_title + "::" + root_definition_name) << "\"";
        if (!root_definition_name.empty())
        {
            os << ",\n";
            os << "  \"$ref\": \"#/definitions/" << root_definition_name << "\"";
        }
        os << ",\n";
        os << "  \"definitions\": \n";
        os << "  {\n";
        os << ")canopy_json\";\n";
        os << "    bool has_definition = false;\n";
    }

    void write_schema_document_finish(std::ostream& os)
    {
        os << "    schema += R\"canopy_json(\n";
        os << "  }\n";
        os << "}\n";
        os << ")canopy_json\";\n";
        os << "    return schema;\n";
    }

    void collect_schema_interfaces(
        const class_entity& current_entity,
        std::vector<const class_entity*>& interfaces)
    {
        if (current_entity.is_in_import() || current_entity.get_is_template())
            return;

        const auto current_type = current_entity.get_entity_type();
        if (current_type == entity_type::INTERFACE && !get_qualified_name(current_entity).empty())
            interfaces.push_back(&current_entity);

        auto members_to_get = entity_type::TYPE_NULL;
        if (current_type == entity_type::NAMESPACE || current_entity.get_owner() == nullptr
            || current_entity.get_name() == "__global__")
        {
            members_to_get = entity_type::NAMESPACE_MEMBERS;
        }
        else if (current_type == entity_type::STRUCT || current_type == entity_type::CLASS
                 || current_type == entity_type::INTERFACE)
        {
            members_to_get = entity_type::STRUCTURE_MEMBERS | entity_type::NAMESPACE_MEMBERS;
        }

        if (members_to_get == entity_type::TYPE_NULL)
            return;

        for (const auto& element_ptr : current_entity.get_elements(members_to_get))
        {
            if (!element_ptr || element_ptr->is_in_import())
                continue;
            const auto* child_class = dynamic_cast<const class_entity*>(element_ptr.get());
            if (!child_class)
                continue;
            collect_schema_interfaces(*child_class, interfaces);
        }
    }

    struct InterfaceMethodSchemaInfo
    {
        std::string method_name;
        std::string send_definition_name;
        std::string receive_definition_name;
        std::string local_send_definition_name;
        std::string local_receive_definition_name;
    };

    std::vector<InterfaceMethodSchemaInfo> get_interface_method_schema_infos(const class_entity& interface_entity)
    {
        std::vector<InterfaceMethodSchemaInfo> infos;
        const auto interface_definition_name = get_qualified_name(interface_entity);
        for (const auto& element_ptr : interface_entity.get_elements(entity_type::FUNCTION_METHOD))
        {
            if (!element_ptr || element_ptr->get_entity_type() != entity_type::FUNCTION_METHOD)
                continue;
            const auto* method = dynamic_cast<const function_entity*>(element_ptr.get());
            if (!method)
                continue;

            const auto method_name = generator::clean_type_name(method->get_name());
            if (method_name.empty())
                continue;

            infos.push_back(
                InterfaceMethodSchemaInfo{FLD(method_name) method_name,
                    FLD(send_definition_name) interface_definition_name + "_" + method_name + "_send",
                    FLD(receive_definition_name) interface_definition_name + "_" + method_name + "_receive",
                    FLD(local_send_definition_name) method_name + "_send",
                    FLD(local_receive_definition_name) method_name + "_receive"});
        }
        return infos;
    }

    std::string write_interface_inner_schema_body(const class_entity& interface_entity)
    {
        const auto method_infos = get_interface_method_schema_infos(interface_entity);
        std::stringstream body_stream;
        json_writer writer(body_stream);
        writer.open_object();

        const auto description = interface_entity.get_value("description");
        if (!description.empty())
            writer.write_string_property("description", description);

        writer.write_string_property("type", "object");
        writer.write_key("properties");
        writer.open_object();
        for (const auto& method_info : method_infos)
        {
            writer.write_key(method_info.method_name);
            writer.open_object();
            writer.write_string_property("type", "object");
            writer.write_key("properties");
            writer.open_object();
            writer.write_key("send");
            writer.open_object();
            writer.write_string_property("$ref", "#/definitions/" + method_info.local_send_definition_name);
            writer.close_object();
            writer.write_key("receive");
            writer.open_object();
            writer.write_string_property("$ref", "#/definitions/" + method_info.local_receive_definition_name);
            writer.close_object();
            writer.close_object();
            writer.write_key("required");
            writer.open_array();
            writer.write_array_string_element("send");
            writer.write_array_string_element("receive");
            writer.close_array();
            writer.write_raw_property("additionalProperties", "false");
            writer.close_object();
        }
        writer.close_object();

        if (!method_infos.empty())
        {
            writer.write_key("required");
            writer.open_array();
            for (const auto& method_info : method_infos)
                writer.write_array_string_element(method_info.method_name);
            writer.close_array();
        }

        writer.write_raw_property("additionalProperties", "false");
        writer.close_object();
        return body_stream.str();
    }

    // Entry point function
    // ** RESTORED **
    void write_json_schema(
        const class_entity& root_entity,
        std::ostream& os,
        const std::string& schema_title,
        const schema_profile& profile)
    {
        write_json_schema_document(root_entity, os, schema_title, {}, profile);
    }

    void write_cpp_schema_accessors(
        const class_entity& root_entity,
        std::ostream& os,
        const std::string& schema_title,
        const std::string& schema_function_name,
        const schema_profile& profile)
    {
        active_profile_scope profile_guard(profile);
        std::vector<OrderedDefinitionItem> ordered_defs;
        std::map<std::string, DefinitionInfoVariant> definition_info_map;
        collect_definition_info(root_entity, ordered_defs, definition_info_map);

        std::set<std::string> root_definition_names;
        std::vector<std::pair<std::string, const class_entity*>> accessors;
        for (const auto& item : ordered_defs)
        {
            root_definition_names.insert(item.first);
            if (const auto* ent = get_struct_accessor_entity(item.first, definition_info_map); ent)
                accessors.push_back({item.first, ent});
        }
        std::vector<const class_entity*> interface_accessors;
        collect_schema_interfaces(root_entity, interface_accessors);

        const auto all_definition_names
            = collect_definition_closure(root_entity, root_definition_names, definition_info_map);
        const auto all_definition_bodies
            = collect_definition_bodies(root_entity, all_definition_names, definition_info_map);
        const auto detail_namespace = sanitise_cpp_identifier(schema_function_name) + "_detail";

        os << "namespace canopy\n";
        os << "{\n";
        os << "    namespace generated_schema\n";
        os << "    {\n";
        os << "        namespace " << detail_namespace << "\n";
        os << "        {\n";
        os << "            inline void append_definition(\n";
        os << "                std::string& schema,\n";
        os << "                const char* definition_name,\n";
        os << "                const char* definition_body,\n";
        os << "                bool& has_definition)\n";
        os << "            {\n";
        os << "                if (has_definition)\n";
        os << "                    schema += \",\";\n";
        os << "                schema += \"\\n    \\\"\";\n";
        os << "                schema += definition_name;\n";
        os << "                schema += \"\\\": \";\n";
        os << "                schema += definition_body;\n";
        os << "                has_definition = true;\n";
        os << "            }\n\n";

        for (const auto& definition_name : all_definition_names)
        {
            if (get_struct_accessor_entity(definition_name, definition_info_map))
                continue;

            os << "            inline constexpr const char* " << sanitise_cpp_identifier(definition_name)
               << "_inner_schema()\n";
            os << "            {\n";
            os << "                return R\"canopy_json(";
            os << all_definition_bodies.at(definition_name);
            os << ")canopy_json\";\n";
            os << "            }\n\n";
        }
        os << "        }\n\n";
        os << "        inline std::string " << schema_function_name << "()\n";
        os << "        {\n";
        write_schema_document_start(os, schema_title, {}, profile);
        for (const auto& definition_name : all_definition_names)
        {
            os << "            " << detail_namespace << "::append_definition(\n";
            os << "                schema,\n";
            os << "                \"" << definition_name << "\",\n";
            os << "                "
               << get_definition_inner_schema_expression(definition_name, detail_namespace, definition_info_map) << ",\n";
            os << "                has_definition);\n";
        }
        write_schema_document_finish(os);
        os << "        }\n";
        os << "    }\n";
        os << "}\n";

        for (const auto& accessor : accessors)
        {
            const auto namespaces = get_cpp_namespace_parts(*accessor.second);
            for (const auto& ns : namespaces)
            {
                os << "\nnamespace " << ns << "\n";
                os << "{\n";
            }

            const auto member_scope_name = get_cpp_member_scope_name(*accessor.second);
            const auto definition_names = collect_definition_closure(root_entity, {accessor.first}, definition_info_map);
            const auto definition_bodies = collect_definition_bodies(root_entity, definition_names, definition_info_map);
            os << "\nconstexpr const char* " << member_scope_name << "::get_inner_schema()\n";
            os << "{\n";
            os << "    return R\"canopy_json(";
            os << definition_bodies.at(accessor.first);
            os << ")canopy_json\";\n";
            os << "}\n\n";
            os << "\ninline std::string " << member_scope_name << "::get_schema()\n";
            os << "{\n";
            os << "    return get_schema(::rpc::encoding::yas_json);\n";
            os << "}\n\n";
            os << "inline std::string " << member_scope_name << "::get_schema(::rpc::encoding encoding)\n";
            os << "{\n";
            os << "    if (encoding != ::rpc::encoding::yas_json)\n";
            os << "        throw std::invalid_argument(\"JSON schema is only available as "
                  "rpc::encoding::yas_json\");\n";
            write_schema_document_start(os, schema_title, accessor.first, profile);
            for (const auto& definition_name : definition_names)
            {
                os << "    ::canopy::generated_schema::" << detail_namespace << "::append_definition(\n";
                os << "        schema,\n";
                os << "        \"" << definition_name << "\",\n";
                os << "        "
                   << get_definition_inner_schema_expression(definition_name, detail_namespace, definition_info_map)
                   << ",\n";
                os << "        has_definition);\n";
            }
            write_schema_document_finish(os);
            os << "}\n";
            // Flavor-aware overload (Phase 1): delegates to encoding-only version.
            // MCP profile transforms (no defaults, string-only enums) require separate
            // definition bodies and are deferred; for now config and mcp share the
            // same schema document. The profile seam exists so transforms can be added
            // without changing the public API surface.
            os << "\ninline std::string " << member_scope_name
               << "::get_schema(::rpc::encoding encoding, ::rpc::schema_flavor flavor)\n";
            os << "{\n";
            os << "    if (flavor == ::rpc::schema_flavor::config)\n";
            os << "        return get_schema(encoding);\n";
            os << "    // MCP flavor: reuses config schema for now; full MCP profile\n";
            os << "    // (no defaults, string-only enums) requires separate definition bodies.\n";
            os << "    return get_schema(encoding);\n";
            os << "}\n";

            for (auto it = namespaces.rbegin(); it != namespaces.rend(); ++it)
            {
                os << "} // namespace " << *it << "\n";
            }
        }

        for (const auto* interface_accessor : interface_accessors)
        {
            const auto namespaces = get_cpp_namespace_parts(*interface_accessor);
            for (const auto& ns : namespaces)
            {
                os << "\nnamespace " << ns << "\n";
                os << "{\n";
            }

            const auto member_scope_name = get_cpp_member_scope_name(*interface_accessor);
            const auto interface_definition_name = get_qualified_name(*interface_accessor);
            const auto method_infos = get_interface_method_schema_infos(*interface_accessor);
            std::set<std::string> method_global_definition_names;
            for (const auto& method_info : method_infos)
            {
                method_global_definition_names.insert(method_info.send_definition_name);
                method_global_definition_names.insert(method_info.receive_definition_name);
            }
            const auto interface_definition_names
                = collect_definition_closure(root_entity, method_global_definition_names, definition_info_map);

            os << "\nconstexpr const char* " << member_scope_name << "::get_inner_schema()\n";
            os << "{\n";
            os << "    return R\"canopy_json(";
            os << write_interface_inner_schema_body(*interface_accessor);
            os << ")canopy_json\";\n";
            os << "}\n\n";
            os << "\ninline std::string " << member_scope_name << "::get_schema()\n";
            os << "{\n";
            os << "    return get_schema(::rpc::encoding::yas_json);\n";
            os << "}\n\n";
            os << "inline std::string " << member_scope_name << "::get_schema(::rpc::encoding encoding)\n";
            os << "{\n";
            os << "    if (encoding != ::rpc::encoding::yas_json)\n";
            os << "        throw std::invalid_argument(\"JSON schema is only available as "
                  "rpc::encoding::yas_json\");\n";
            write_schema_document_start(os, schema_title, interface_definition_name, profile);
            os << "    ::canopy::generated_schema::" << detail_namespace << "::append_definition(\n";
            os << "        schema,\n";
            os << "        \"" << interface_definition_name << "\",\n";
            os << "        " << get_cpp_fully_qualified_scope_name(*interface_accessor) << "::get_inner_schema(),\n";
            os << "        has_definition);\n";
            for (const auto& method_info : method_infos)
            {
                os << "    ::canopy::generated_schema::" << detail_namespace << "::append_definition(\n";
                os << "        schema,\n";
                os << "        \"" << method_info.local_send_definition_name << "\",\n";
                os << "        "
                   << get_definition_inner_schema_expression(
                          method_info.send_definition_name, detail_namespace, definition_info_map)
                   << ",\n";
                os << "        has_definition);\n";
                os << "    ::canopy::generated_schema::" << detail_namespace << "::append_definition(\n";
                os << "        schema,\n";
                os << "        \"" << method_info.local_receive_definition_name << "\",\n";
                os << "        "
                   << get_definition_inner_schema_expression(
                          method_info.receive_definition_name, detail_namespace, definition_info_map)
                   << ",\n";
                os << "        has_definition);\n";
            }
            for (const auto& definition_name : interface_definition_names)
            {
                if (method_global_definition_names.count(definition_name))
                    continue;
                os << "    ::canopy::generated_schema::" << detail_namespace << "::append_definition(\n";
                os << "        schema,\n";
                os << "        \"" << definition_name << "\",\n";
                os << "        "
                   << get_definition_inner_schema_expression(definition_name, detail_namespace, definition_info_map)
                   << ",\n";
                os << "        has_definition);\n";
            }
            write_schema_document_finish(os);
            os << "}\n";

            // Flavor-aware overload (Phase 1): delegates to the encoding-only
            // version. MCP profile transforms (no defaults, string-only enums)
            // need separate definition bodies and are deferred; config and mcp
            // share the document for now. The seam keeps the public API stable.
            os << "\ninline std::string " << member_scope_name
               << "::get_schema(::rpc::encoding encoding, ::rpc::schema_flavor flavor)\n";
            os << "{\n";
            os << "    (void)flavor;\n";
            os << "    return get_schema(encoding);\n";
            os << "}\n";

            for (auto it = namespaces.rbegin(); it != namespaces.rend(); ++it)
            {
                os << "} // namespace " << *it << "\n";
            }
        }
    }

    // --- from_json_object / to_json_object converter generation ---------------

    namespace convert
    {
        struct field_info
        {
            std::string name;
            std::string raw_type;        // verbatim IDL type, suitable for pasting into C++.
            std::string cleaned_type;    // whitespace-stripped form used for classification.
            std::string default_value;   // verbatim text after `=` in the IDL; empty if none.
            bool is_optional = false;    // true iff raw_type is rpc::optional<...>.
            std::string optional_inner;  // inner T when is_optional, raw form.
            bool is_raw_json = false;    // true iff raw_type is json::v1::object (passthrough).
        };

        inline std::string cpp_string_literal(const std::string& value)
        {
            std::string out;
            out.reserve(value.size() + 2);
            out += '"';
            for (const char ch : value)
            {
                switch (ch)
                {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += ch;
                    break;
                }
            }
            out += '"';
            return out;
        }

        inline std::string strip_outer_braces(std::string value)
        {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.erase(value.begin());
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.pop_back();
            return value;
        }

        // Extract the inner type T from `rpc::optional<T>` using the raw IDL
        // type text so we preserve whatever namespace qualification the author
        // wrote. parse_template_args strips whitespace through clean_type_name,
        // which is fine for classification but loses the verbatim form needed
        // for pasting into the generated C++.
        inline bool split_optional_raw(
            const std::string& raw_type,
            std::string& inner_raw)
        {
            const auto open = raw_type.find('<');
            const auto close = raw_type.rfind('>');
            if (open == std::string::npos || close == std::string::npos || close <= open + 1)
                return false;

            inner_raw = raw_type.substr(open + 1, close - open - 1);
            // Trim surrounding whitespace.
            const auto first = inner_raw.find_first_not_of(" \t\n\r");
            const auto last = inner_raw.find_last_not_of(" \t\n\r");
            if (first == std::string::npos)
                return false;
            inner_raw = inner_raw.substr(first, last - first + 1);
            return !inner_raw.empty();
        }

        // Returns true when the convert.h runtime has a matching overload, or
        // when the type recursively reduces to types that do. Structs with any
        // unsupported field are skipped to keep the generated converter code
        // honest about what it can round-trip.
        inline bool field_type_is_supportable(
            const std::string& cleaned_type,
            const taggable_idl_type_set& taggable_idl_types)
        {
            // Raw pointers and references can't carry through JSON.
            if (cleaned_type.find('*') != std::string::npos)
                return false;
            if (cleaned_type.find('&') != std::string::npos)
                return false;
            // rpc::shared_ptr is an RPC marshalling primitive, not a JSON
            // configuration type.
            if (cleaned_type.find("rpc::shared_ptr<") != std::string::npos
                || cleaned_type.find("::rpc::shared_ptr<") != std::string::npos)
                return false;
            // std::queue / std::stack are container adaptors with no element
            // iteration in the C++ standard; the schema treats them as arrays
            // but convert.h does not (yet) have round-trip overloads.
            for (const auto* needle : {"std::queue<", "std::stack<"})
            {
                if (cleaned_type.find(needle) != std::string::npos)
                    return false;
            }

            // rpc::variant<...> alternatives must all have a canonical tag and
            // the tags must be distinct, otherwise the tag-keyed JSON shape
            // can't disambiguate them. The runtime template trait would also
            // fail to compile, so block here for a clean codegen-time error.
            std::vector<std::string> variant_alts;
            if (is_rpc_variant_schema_type(cleaned_type, variant_alts))
            {
                std::vector<std::string> tags;
                if (!variant_alternatives_have_unique_tags(variant_alts, taggable_idl_types, tags))
                    return false;
            }

            // std::map / std::unordered_map are only supported with string
            // keys (JSON limitation). Inspect the first template arg.
            for (const auto* needle : {"std::map<", "std::unordered_map<"})
            {
                const auto pos = cleaned_type.find(needle);
                if (pos == std::string::npos)
                    continue;
                std::string container;
                std::vector<std::string> args;
                if (!parse_template_args(cleaned_type.substr(pos), container, args) || args.empty())
                    return false;
                auto key_type = args.front();
                if (key_type.rfind("::", 0) == 0)
                    key_type.erase(0, 2);
                if (key_type != "std::string" && key_type != "string")
                    return false;
            }

            return true;
        }

        struct collect_result
        {
            std::vector<field_info> fields;
            bool supported = true;
        };

        inline collect_result collect_struct_fields(
            const class_entity& ent,
            const taggable_idl_type_set& taggable_idl_types)
        {
            collect_result result;
            // Walk the full member list so we can include both public and
            // private fields. Access to private members is granted via the
            // friend declarations emitted alongside the struct definition in
            // the IDL-generated header.
            using underlying = std::underlying_type<entity_type>::type;
            const auto all_members = static_cast<entity_type>(
                static_cast<underlying>(entity_type::STRUCTURE_MEMBERS));
            for (const auto& element : ent.get_elements(all_members))
            {
                if (!element)
                    continue;
                if (element->get_entity_type() == entity_type::FUNCTION_PRIVATE
                    || element->get_entity_type() == entity_type::FUNCTION_PUBLIC)
                    continue;
                if (element->get_entity_type() != entity_type::FUNCTION_VARIABLE)
                    continue;

                const auto* var = dynamic_cast<const function_entity*>(element.get());
                if (!var || var->is_static())
                    continue;

                field_info info;
                info.name = var->get_name();
                info.raw_type = var->get_return_type();
                info.default_value = var->get_default_value();
                if (info.name.empty() || info.raw_type.empty())
                    continue;

                info.cleaned_type = generator::clean_type_name(info.raw_type);
                if (info.cleaned_type.empty())
                    continue;

                if (!field_type_is_supportable(info.cleaned_type, taggable_idl_types))
                {
                    result.supported = false;
                    return result;
                }

                std::string optional_inner_cleaned;
                if (is_optional_schema_type(info.cleaned_type, optional_inner_cleaned))
                {
                    info.is_optional = true;
                    if (!split_optional_raw(info.raw_type, info.optional_inner))
                        info.optional_inner = optional_inner_cleaned;
                }
                else if (is_json_dom_schema_any_type(info.cleaned_type))
                {
                    // Raw JSON passthrough: null is a legitimate value, not
                    // an "absent" marker.
                    info.is_raw_json = true;
                }

                result.fields.push_back(std::move(info));
            }
            return result;
        }

        inline void write_field_reader(
            std::ostream& os,
            const field_info& field,
            const std::string& struct_label,
            const std::string& indent)
        {
            // Calls inside the function body rely on the using-declarations
            // emitted at the top, so ADL finds converters defined in the IDL's
            // own namespace alongside the primitives in json::v1::convert.
            const auto value_var = "__it->second";
            const auto present_check = std::string("__it != __map.end() && ") + value_var
                + ".get_type() != ::json::v1::object::type::null_type";

            os << indent << "{\n";
            os << indent << "    const auto __it = __map.find(\"" << field.name << "\");\n";

            if (field.is_optional)
            {
                if (field.default_value.empty())
                {
                    os << indent << "    if (" << present_check << ")\n";
                    os << indent << "        __result." << field.name << " = from_json_object<"
                       << field.optional_inner << ">(" << value_var << ");\n";
                }
                else
                {
                    os << indent << "    if (__it == __map.end())\n";
                    os << indent << "        __result." << field.name << " = " << field.optional_inner << "{"
                       << strip_outer_braces(field.default_value) << "};\n";
                    os << indent << "    else if (" << value_var
                       << ".get_type() != ::json::v1::object::type::null_type)\n";
                    os << indent << "        __result." << field.name << " = from_json_object<"
                       << field.optional_inner << ">(" << value_var << ");\n";
                }
            }
            else if (field.is_raw_json)
            {
                // json::v1::object accepts any JSON value including null, so
                // key presence is the only signal of "user supplied this".
                os << indent << "    if (__it != __map.end())\n";
                os << indent << "        __result." << field.name << " = from_json_object<" << field.raw_type
                   << ">(" << value_var << ");\n";
                if (field.default_value.empty())
                {
                    os << indent << "    else\n";
                    os << indent << "        throw ::json::v1::config_error(" << cpp_string_literal(
                        struct_label + ": required field '" + field.name + "' is missing")
                       << ");\n";
                }
                else
                {
                    os << indent << "    else\n";
                    os << indent << "        __result." << field.name << " = " << field.default_value << ";\n";
                }
            }
            else
            {
                os << indent << "    if (" << present_check << ")\n";
                os << indent << "        __result." << field.name << " = from_json_object<" << field.raw_type
                   << ">(" << value_var << ");\n";
                if (field.default_value.empty())
                {
                    os << indent << "    else\n";
                    os << indent << "        throw ::json::v1::config_error(" << cpp_string_literal(
                        struct_label + ": required field '" + field.name + "' is missing")
                       << ");\n";
                }
                else
                {
                    os << indent << "    else\n";
                    os << indent << "        __result." << field.name << " = " << field.default_value << ";\n";
                }
            }
            os << indent << "}\n";
        }

        inline void write_field_writer(
            std::ostream& os,
            const field_info& field,
            const std::string& indent)
        {
            if (field.is_optional)
            {
                os << indent << "if (__value." << field.name << ".has_value())\n";
                os << indent << "    __map.emplace(\"" << field.name << "\", to_json_object(*__value."
                   << field.name << "));\n";
            }
            else
            {
                os << indent << "__map.emplace(\"" << field.name << "\", to_json_object(__value." << field.name
                   << "));\n";
            }
        }

        struct enum_value_info
        {
            std::string name;
            int64_t value = 0;
        };

        inline std::vector<enum_value_info> collect_enum_values(const class_entity& ent)
        {
            std::vector<enum_value_info> values;
            using underlying = std::underlying_type<entity_type>::type;
            const auto all_members = static_cast<entity_type>(
                static_cast<underlying>(entity_type::NAMESPACE_MEMBERS)
                | static_cast<underlying>(entity_type::STRUCTURE_MEMBERS));

            int64_t next_value = 0;
            for (const auto& element : ent.get_elements(all_members))
            {
                if (!element)
                    continue;
                std::string name = generator::clean_type_name(element->get_name());
                if (name.empty() || name.find_first_of("{}[]() \t\n\r") != std::string::npos)
                    continue;

                int64_t assigned = next_value;
                if (const auto* value_entity = dynamic_cast<const function_entity*>(element.get()))
                {
                    const auto explicit_str = generator::clean_type_name(value_entity->get_return_type());
                    if (!explicit_str.empty())
                    {
                        try
                        {
                            assigned = std::stoll(explicit_str);
                        }
                        catch (const std::exception&)
                        {
                            // Fall back to the running counter; the schema
                            // generator path already logs malformed values.
                        }
                    }
                }
                values.push_back({std::move(name), assigned});
                next_value = assigned + 1;
            }
            return values;
        }

        inline void write_converter_for_enum(
            std::ostream& os,
            const class_entity& ent)
        {
            const auto namespaces = get_cpp_namespace_parts(ent);
            const auto member_scope_name = get_cpp_member_scope_name(ent);
            const auto qualified_label = [&]
            {
                std::string label;
                for (const auto& ns : namespaces)
                {
                    label += ns;
                    label += "::";
                }
                label += member_scope_name;
                return label;
            }();

            const auto values = collect_enum_values(ent);
            if (values.empty())
                return;

            for (const auto& ns : namespaces)
            {
                os << "\nnamespace " << ns << "\n";
                os << "{\n";
            }

            // Accept either the string name (preferred) or the underlying
            // integer so JSON written by tools that don't know the names can
            // still parse. The string form is the canonical wire shape.
            os << "\ninline " << member_scope_name << " from_json_object(\n";
            os << "    ::json::v1::convert::tag<" << member_scope_name << ">,\n";
            os << "    const ::json::v1::object& __value)\n";
            os << "{\n";
            os << "    if (__value.get_type() == ::json::v1::object::type::string_type)\n";
            os << "    {\n";
            os << "        const auto __name = __value.get<std::string>();\n";
            for (const auto& v : values)
            {
                os << "        if (__name == \"" << v.name << "\") return " << member_scope_name << "::" << v.name
                   << ";\n";
            }
            os << "        throw ::json::v1::config_error(" << cpp_string_literal(qualified_label + ": unknown enum value '")
               << " + __name + \"'\");\n";
            os << "    }\n";
            os << "    if (__value.get_type() == ::json::v1::object::type::number_type)\n";
            os << "    {\n";
            os << "        switch (__value.get<int64_t>())\n";
            os << "        {\n";
            for (const auto& v : values)
            {
                os << "        case " << v.value << ": return " << member_scope_name << "::" << v.name << ";\n";
            }
            os << "        }\n";
            os << "        throw ::json::v1::config_error("
               << cpp_string_literal(qualified_label + ": numeric value is not a known enum member") << ");\n";
            os << "    }\n";
            os << "    throw ::json::v1::config_error("
               << cpp_string_literal(qualified_label + ": expected JSON string or integer for enum") << ");\n";
            os << "}\n\n";

            os << "inline ::json::v1::object to_json_object(" << member_scope_name << " __value)\n";
            os << "{\n";
            os << "    switch (__value)\n";
            os << "    {\n";
            for (const auto& v : values)
            {
                os << "    case " << member_scope_name << "::" << v.name << ": return ::json::v1::object(std::string(\""
                   << v.name << "\"));\n";
            }
            os << "    }\n";
            os << "    throw ::json::v1::config_error("
               << cpp_string_literal(qualified_label + ": value is not a known enum member") << ");\n";
            os << "}\n";

            for (auto it = namespaces.rbegin(); it != namespaces.rend(); ++it)
            {
                os << "} // namespace " << *it << "\n";
            }
        }

        inline void write_converter_for_struct(
            std::ostream& os,
            const class_entity& ent,
            const taggable_idl_type_set& taggable_idl_types)
        {
            const auto namespaces = get_cpp_namespace_parts(ent);
            const auto member_scope_name = get_cpp_member_scope_name(ent);
            const auto qualified_label = [&]
            {
                std::string label;
                for (const auto& ns : namespaces)
                {
                    label += ns;
                    label += "::";
                }
                label += member_scope_name;
                return label;
            }();

            const auto collected = collect_struct_fields(ent, taggable_idl_types);
            if (!collected.supported)
                return;
            const auto& fields = collected.fields;

            for (const auto& ns : namespaces)
            {
                os << "\nnamespace " << ns << "\n";
                os << "{\n";
            }

            os << "\ninline " << member_scope_name << " from_json_object(\n";
            os << "    ::json::v1::convert::tag<" << member_scope_name << ">,\n";
            os << "    const ::json::v1::object& __value)\n";
            os << "{\n";
            // Bring the dispatcher into scope so unqualified from_json_object<T>
            // calls pick up both primitive overloads and ADL-discovered ones
            // in user namespaces.
            os << "    using ::json::v1::convert::from_json_object;\n";
            os << "    if (__value.get_type() != ::json::v1::object::type::map_type)\n";
            os << "        throw ::json::v1::config_error(" << cpp_string_literal(qualified_label + ": expected JSON object")
               << ");\n";
            os << "    const auto& __map = __value.as_map();\n";
            os << "    " << member_scope_name << " __result{};\n";

            for (const auto& field : fields)
            {
                write_field_reader(os, field, qualified_label, "    ");
            }

            if (fields.empty())
                os << "    (void)__map;\n";
            os << "    return __result;\n";
            os << "}\n\n";

            os << "inline ::json::v1::object to_json_object(const " << member_scope_name << "& __value)\n";
            os << "{\n";
            // Bring primitive writers into scope so unqualified calls compose
            // with ADL-discovered IDL converters.
            os << "    using ::json::v1::convert::to_json_object;\n";
            if (fields.empty())
                os << "    (void)__value;\n";
            os << "    ::json::v1::map __map;\n";
            for (const auto& field : fields)
            {
                write_field_writer(os, field, "    ");
            }
            os << "    return ::json::v1::object(std::move(__map));\n";
            os << "}\n";

            for (auto it = namespaces.rbegin(); it != namespaces.rend(); ++it)
            {
                os << "} // namespace " << *it << "\n";
            }
        }
    } // namespace convert

    bool struct_will_have_converter(const class_entity& ent)
    {
        if (ent.get_entity_type() != entity_type::STRUCT || ent.get_is_template())
            return false;
        // Derive the taggable set from the AST root rather than threading it
        // through every caller; struct_will_have_converter is invoked at
        // codegen time when only the struct entity is in hand.
        const class_entity* root = &ent;
        while (root->get_owner() != nullptr && root->get_owner()->get_name() != "__global__")
            root = root->get_owner();
        const auto taggable = collect_taggable_idl_types(*root);
        return convert::collect_struct_fields(ent, taggable).supported;
    }

    void write_cpp_convert_accessors(
        const class_entity& root_entity,
        std::ostream& os)
    {
        std::vector<OrderedDefinitionItem> ordered_defs;
        std::map<std::string, DefinitionInfoVariant> definition_info_map;
        collect_definition_info(root_entity, ordered_defs, definition_info_map);

        const auto taggable = collect_taggable_idl_types(root_entity);
        std::vector<const class_entity*> structs;
        std::vector<const class_entity*> enums;
        for (const auto& item : ordered_defs)
        {
            if (const auto* ent = get_struct_accessor_entity(item.first, definition_info_map); ent)
            {
                structs.push_back(ent);
            }
            else if (std::holds_alternative<const class_entity*>(item.second))
            {
                const auto* enum_ent = std::get<const class_entity*>(item.second);
                if (enum_ent && enum_ent->get_entity_type() == entity_type::ENUM)
                    enums.push_back(enum_ent);
            }
        }

        if (structs.empty() && enums.empty())
            return;

        os << "\n// --- Generated json::v1::object <-> IDL converters ---\n";
        for (const auto* ent : enums)
        {
            convert::write_converter_for_enum(os, *ent);
        }
        for (const auto* ent : structs)
        {
            convert::write_converter_for_struct(os, *ent, taggable);
        }
    }

} // namespace json_schema
