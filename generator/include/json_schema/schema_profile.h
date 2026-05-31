/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>

namespace json_schema
{
    // Selects the audience/shape of a generated JSON schema. The same canonical
    // IDL model drives every flavor; the profile below only gates emission, so a
    // single emitter can serve both editor/config tooling and minimal MCP tool
    // schemas without a second code path.
    //
    // IMPORTANT: `config` is the default everywhere. MCP-specific transforms
    // (string-only enums, dropped defaults, inlined refs, no $id) are properties
    // of `mcp_profile()` only and must never leak into the configuration path.
    enum class schema_flavor
    {
        config, // full authoring schema: descriptions, defaults, string|int enums, $id
        mcp     // minimal tool schema: inline, string-only enums, no defaults/$id
    };

    struct schema_profile
    {
        enum class required_policy
        {
            idl_accurate, // non-optional fields without an explicit default
            none
        };
        enum class enum_form
        {
            string_only,
            string_and_int
        };
        enum class ref_form
        {
            inline_all,
            external_id
        };

        bool include_descriptions = true;
        bool include_defaults = true;
        required_policy required = required_policy::idl_accurate;
        enum_form enums = enum_form::string_and_int;
        ref_form refs = ref_form::external_id;
        bool emit_id = true;
        bool additional_properties_false = true;

        // Stable $id = id_base + id_path. $id is emitted only when `emit_id` is
        // set AND `id_path` is non-empty, so existing callers (which leave
        // id_path empty) produce byte-identical output.
        std::string id_base;
        std::string id_path;
    };

    // The default profile: reproduces the configuration schema exactly as it is
    // generated today. Used as the default argument for every public entry point.
    schema_profile config_strict_profile();

    // Minimal, self-contained schema for MCP/tool consumers.
    schema_profile mcp_profile();
} // namespace json_schema
