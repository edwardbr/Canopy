/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include "coreclasses.h" // Your parser API header
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace json_schema
{

    void write_json_schema(
        const class_entity& root_entity,
        std::ostream& os,
        const std::string& schema_title = "Generated Schema");

    void write_cpp_schema_accessors(
        const class_entity& root_entity,
        std::ostream& os,
        const std::string& schema_title,
        const std::string& schema_function_name);

    // Emits ADL overloads `from_json_object(tag<T>, const json::v1::object&)`
    // and `to_json_object(const T&)` for each non-template IDL struct reachable
    // from `root_entity`. Missing required fields fall back to the IDL default
    // when one is declared; otherwise the converter throws json::v1::config_error.
    void write_cpp_convert_accessors(
        const class_entity& root_entity,
        std::ostream& os);

    // Returns true iff `ent` is a non-template struct whose fields are all
    // supported by the runtime convert.h surface — i.e. iff
    // write_cpp_convert_accessors will emit a converter for it. The
    // synchronous_generator uses this to gate friend declarations on the
    // struct body so a friend never references an undefined ADL overload.
    bool struct_will_have_converter(const class_entity& ent);

    // The set of IDL-defined types that the synchronous_generator will emit a
    // variant_alternative_tag specialization for — i.e. the unqualified names
    // of every non-template struct and enum reachable from the IDL. The tag
    // resolver consults this set so it never claims a tag the runtime trait
    // can't supply (e.g. json::v1::object, typedef aliases like
    // `int`/`error_code`, or any other non-IDL non-primitive type).
    using taggable_idl_type_set = std::set<std::string>;

    // Collect the set above from the IDL AST.
    taggable_idl_type_set collect_taggable_idl_types(const class_entity& root_entity);

    // Tag-name resolution for an rpc::variant alternative type, shared
    // between the global JSON-schema generator and the per-function schema
    // generator so both emit the same canonical {"<tag>": value} shape.
    // Returns empty string when no canonical tag exists — template
    // instantiations, json::v1::object, typedef aliases, or any type the
    // runtime trait can't be specialized for.
    std::string variant_alternative_tag_for(
        std::string alternative_type,
        const taggable_idl_type_set& taggable_idl_types);

    // True iff every alternative has a tag and no two share one. Populates
    // `out_tags` in parallel with `alternative_types`.
    bool variant_alternatives_have_unique_tags(
        const std::vector<std::string>& alternative_types,
        const taggable_idl_type_set& taggable_idl_types,
        std::vector<std::string>& out_tags);

} // namespace json_schema
