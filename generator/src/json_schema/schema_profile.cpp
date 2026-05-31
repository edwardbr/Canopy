/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "json_schema/schema_profile.h"

namespace json_schema
{
    schema_profile config_strict_profile()
    {
        // The struct's member defaults already describe today's configuration
        // schema, so a default-constructed profile is the strict config profile.
        return schema_profile{};
    }

    schema_profile mcp_profile()
    {
        schema_profile p;
        p.include_defaults = false;
        p.enums = schema_profile::enum_form::string_only;
        p.refs = schema_profile::ref_form::inline_all;
        p.emit_id = false;
        // required stays idl_accurate (the minimal set is correct for tools too);
        // descriptions are kept (they guide the model); additionalProperties stays
        // false (catches malformed tool calls).
        return p;
    }
} // namespace json_schema
