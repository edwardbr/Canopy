/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>
#include <string_view>

#include <json/config.h>
#include <json/convert.h>
#include <json/json_utils.h>
#include <json/schema_validator.h>

namespace json
{
    inline namespace v1
    {
        // Create the initial configuration object declared by schema defaults.
        // Application defaults and user JSON are layered over this before
        // validation and conversion to generated C++/IDL types.
        [[nodiscard]] inline object make_default_config(const object& schema)
        {
            return schema_default_values(schema);
        }

        [[nodiscard]] inline object make_default_config(std::string_view schema_json)
        {
            return make_default_config(parse(schema_json));
        }

        // Boundary helpers for application and test configuration:
        //
        //   JSON schema defaults
        //     < component/library defaults
        //     < config-file or config-blob values
        //     < command-line overrides
        //
        // The merged JSON is validated before conversion, then converted through
        // Canopy's normal YAS JSON path into the generated IDL/C++ type. Code
        // below this boundary should accept the typed options object rather than
        // repeatedly probing raw JSON.
        [[nodiscard]] inline object make_effective_config(
            const object& schema,
            const object& component_defaults,
            const object& config_values,
            const object& command_line_overrides = object(map{}),
            overlay_options overlay = {})
        {
            auto effective_json = make_default_config(schema);
            effective_json = merge_overlay(effective_json, component_defaults, overlay);
            effective_json = merge_overlay(effective_json, config_values, overlay);
            effective_json = merge_overlay(effective_json, command_line_overrides, overlay);

            if (overlay.validate_result)
                schema::schema_validator(schema).validate_or_throw(effective_json);

            return effective_json;
        }

        [[nodiscard]] inline object make_effective_config(
            std::string_view schema_json,
            const object& component_defaults,
            const object& config_values,
            const object& command_line_overrides = object(map{}),
            overlay_options overlay = {})
        {
            return make_effective_config(
                parse(schema_json), component_defaults, config_values, command_line_overrides, overlay);
        }

        template<typename Options>
        [[nodiscard]] Options load_typed_config(
            const object& schema,
            const object& component_defaults,
            const object& config_values,
            const object& command_line_overrides = object(map{}),
            overlay_options overlay = {})
        {
            return convert::from_json_object<Options>(
                make_effective_config(schema, component_defaults, config_values, command_line_overrides, overlay));
        }

        template<typename Options>
        [[nodiscard]] Options load_typed_config(
            std::string_view schema_json,
            const object& component_defaults,
            const object& config_values,
            const object& command_line_overrides = object(map{}),
            overlay_options overlay = {})
        {
            return load_typed_config<Options>(
                parse(schema_json), component_defaults, config_values, command_line_overrides, overlay);
        }

        template<typename Options>
        [[nodiscard]] Options load_typed_config_file(
            const object& schema,
            const std::filesystem::path& config_path,
            const object& component_defaults,
            const object& command_line_overrides = object(map{}),
            overlay_options overlay = {})
        {
            return load_typed_config<Options>(
                schema, component_defaults, parse_file(config_path), command_line_overrides, overlay);
        }
    } // namespace v1
} // namespace json
