/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <json/config.h>
#include <json/schema_validator.h>

namespace json
{
    inline namespace v1
    {
        // Configuration overlay helpers are deliberately separate from the
        // schema validator. JSON Schema "default" is only an annotation in the
        // validator; these helpers materialise those defaults before validating
        // the effective object consumed by a component.
        enum class overlay_null_policy
        {
            // Treat null in an override as if the field was not supplied. This
            // matches rpc::optional JSON input semantics for configuration.
            ignore,
            // Preserve null as the final value.
            keep,
            // Remove an existing field from the merged object.
            erase
        };

        struct overlay_options
        {
            overlay_null_policy null_policy{overlay_null_policy::ignore};
            bool validate_result{true};
        };

        // Recursively overlay one JSON object on another. This is configuration
        // layering, not RFC 7396 JSON Merge Patch: object members merge, but
        // scalar/array overrides replace the base value. Configuration code uses
        // ignore-by-default null handling so a CLI/config null behaves like an
        // omitted rpc::optional field.
        [[nodiscard]] inline object merge_overlay(
            const object& base_values,
            const object& override_values,
            overlay_options options = {})
        {
            if (override_values.get_type() == object::type::null_type && options.null_policy == overlay_null_policy::ignore)
            {
                return base_values;
            }

            if (base_values.get_type() != object::type::map_type || override_values.get_type() != object::type::map_type)
                return override_values;

            map merged = base_values.as_map();
            for (const auto& [key, override_value] : override_values.as_map())
            {
                if (override_value.get_type() == object::type::null_type)
                {
                    if (options.null_policy == overlay_null_policy::ignore)
                        continue;
                    if (options.null_policy == overlay_null_policy::erase)
                    {
                        merged.erase(key);
                        continue;
                    }
                }

                auto existing = merged.find(key);
                if (existing != merged.end())
                {
                    existing->second = merge_overlay(existing->second, override_value, options);
                    continue;
                }

                if (override_value.get_type() == object::type::map_type)
                    merged.emplace(key, merge_overlay(object(map{}), override_value, options));
                else
                    merged.emplace(key, override_value);
            }
            return object(std::move(merged));
        }

        namespace detail
        {
            [[nodiscard]] inline const object* map_find(
                const map& values,
                std::string_view key)
            {
                const auto it = values.find(std::string(key));
                if (it == values.end())
                    return nullptr;
                return &it->second;
            }

            [[nodiscard]] inline std::string unescape_pointer_token(std::string_view token)
            {
                std::string result;
                result.reserve(token.size());
                for (size_t i = 0; i < token.size(); ++i)
                {
                    if (token[i] == '~' && i + 1 < token.size())
                    {
                        if (token[i + 1] == '0')
                        {
                            result += '~';
                            ++i;
                            continue;
                        }
                        if (token[i + 1] == '1')
                        {
                            result += '/';
                            ++i;
                            continue;
                        }
                    }
                    result += token[i];
                }
                return result;
            }

            [[nodiscard]] inline const object* resolve_local_ref(
                const object& root_schema,
                std::string_view ref)
            {
                if (ref == "#")
                    return &root_schema;

                if (ref.size() < 2 || ref[0] != '#' || ref[1] != '/')
                    throw config_error("only local JSON schema refs are supported: " + std::string(ref));

                const object* current = &root_schema;
                size_t token_start = 2;
                while (token_start <= ref.size())
                {
                    const auto token_end = ref.find('/', token_start);
                    const auto raw_token = ref.substr(
                        token_start,
                        token_end == std::string_view::npos ? std::string_view::npos : token_end - token_start);
                    const auto token = unescape_pointer_token(raw_token);

                    if (current->get_type() == object::type::map_type)
                    {
                        current = map_find(current->as_map(), token);
                    }
                    else if (current->get_type() == object::type::array_type)
                    {
                        if (token.empty()
                            || !std::all_of(
                                token.begin(), token.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
                        {
                            return nullptr;
                        }
                        const auto index = static_cast<size_t>(std::stoull(token));
                        const auto& values = current->as_array();
                        current = index < values.size() ? &values[index] : nullptr;
                    }
                    else
                    {
                        current = nullptr;
                    }

                    if (!current)
                        return nullptr;
                    if (token_end == std::string_view::npos)
                        break;
                    token_start = token_end + 1;
                }
                return current;
            }

            [[nodiscard]] inline std::optional<object> merge_default_value(
                std::optional<object> base,
                const object& value)
            {
                const overlay_options keep_nulls{overlay_null_policy::keep, false};
                if (base)
                    return merge_overlay(*base, value, keep_nulls);
                return value;
            }

            [[nodiscard]] inline std::optional<object> collect_schema_defaults(
                const object& root_schema,
                const object& schema,
                std::vector<const object*>& active_schemas,
                bool include_optional_properties)
            {
                if (schema.get_type() != object::type::map_type)
                    return std::nullopt;

                // Schemas emitted from IDL can contain recursive definitions.
                // Defaults are only collected from each active schema once so
                // a cyclic $ref graph cannot recurse forever.
                if (std::find(active_schemas.begin(), active_schemas.end(), &schema) != active_schemas.end())
                    return std::nullopt;

                active_schemas.push_back(&schema);
                struct pop_guard
                {
                    explicit pop_guard(std::vector<const object*>& values)
                        : values_(values)
                    {
                    }
                    ~pop_guard() { values_.pop_back(); }
                    std::vector<const object*>& values_;
                } guard(active_schemas);

                const auto& schema_map = schema.as_map();
                std::optional<object> defaults;

                if (const auto* ref = map_find(schema_map, "$ref"))
                {
                    if (ref->get_type() != object::type::string_type)
                        throw config_error("JSON schema $ref must be a string");

                    const auto* resolved = resolve_local_ref(root_schema, ref->get<std::string>());
                    if (!resolved)
                        throw config_error("JSON schema $ref could not be resolved: " + ref->get<std::string>());

                    defaults
                        = collect_schema_defaults(root_schema, *resolved, active_schemas, include_optional_properties);
                }

                if (const auto* all_of = map_find(schema_map, "allOf");
                    all_of && all_of->get_type() == object::type::array_type)
                {
                    for (const auto& child_schema : all_of->as_array())
                    {
                        auto child_defaults = collect_schema_defaults(
                            root_schema, child_schema, active_schemas, include_optional_properties);
                        if (child_defaults)
                            defaults = merge_default_value(std::move(defaults), *child_defaults);
                    }
                }

                if (const auto* properties = map_find(schema_map, "properties");
                    properties && properties->get_type() == object::type::map_type)
                {
                    std::vector<std::string> required_properties;
                    if (const auto* required = map_find(schema_map, "required");
                        required && required->get_type() == object::type::array_type)
                    {
                        for (const auto& required_value : required->as_array())
                        {
                            if (required_value.get_type() == object::type::string_type)
                                required_properties.push_back(required_value.get<std::string>());
                        }
                    }

                    map property_defaults;
                    for (const auto& [property_name, property_schema] : properties->as_map())
                    {
                        auto property_default = collect_schema_defaults(
                            root_schema, property_schema, active_schemas, include_optional_properties);
                        const auto property_is_required
                            = std::find(required_properties.begin(), required_properties.end(), property_name)
                              != required_properties.end();
                        const auto property_has_explicit_default
                            = property_schema.get_type() == object::type::map_type
                              && map_find(property_schema.as_map(), "default") != nullptr;
                        if (property_default
                            && (include_optional_properties || property_is_required || property_has_explicit_default))
                            property_defaults.emplace(property_name, *property_default);
                    }
                    if (!property_defaults.empty())
                        defaults = merge_default_value(std::move(defaults), object(std::move(property_defaults)));
                }

                if (const auto* explicit_default = map_find(schema_map, "default"))
                    defaults = merge_default_value(std::move(defaults), *explicit_default);

                return defaults;
            }

            [[nodiscard]] inline const object* find_property_schema(
                const object& root_schema,
                const object& schema,
                std::string_view property_name,
                std::vector<const object*>& active_schemas)
            {
                if (schema.get_type() != object::type::map_type)
                    return nullptr;

                if (std::find(active_schemas.begin(), active_schemas.end(), &schema) != active_schemas.end())
                    return nullptr;

                active_schemas.push_back(&schema);
                struct pop_guard
                {
                    explicit pop_guard(std::vector<const object*>& values)
                        : values_(values)
                    {
                    }
                    ~pop_guard() { values_.pop_back(); }
                    std::vector<const object*>& values_;
                } guard(active_schemas);

                const auto& schema_map = schema.as_map();
                if (const auto* ref = map_find(schema_map, "$ref"))
                {
                    if (ref->get_type() != object::type::string_type)
                        throw config_error("JSON schema $ref must be a string");

                    const auto* resolved = resolve_local_ref(root_schema, ref->get<std::string>());
                    if (!resolved)
                        throw config_error("JSON schema $ref could not be resolved: " + ref->get<std::string>());

                    if (const auto* property = find_property_schema(root_schema, *resolved, property_name, active_schemas))
                        return property;
                }

                if (const auto* all_of = map_find(schema_map, "allOf");
                    all_of && all_of->get_type() == object::type::array_type)
                {
                    for (const auto& child_schema : all_of->as_array())
                    {
                        if (const auto* property
                            = find_property_schema(root_schema, child_schema, property_name, active_schemas))
                            return property;
                    }
                }

                if (const auto* properties = map_find(schema_map, "properties");
                    properties && properties->get_type() == object::type::map_type)
                {
                    return map_find(properties->as_map(), property_name);
                }

                return nullptr;
            }

            [[nodiscard]] inline object merge_overlay_with_present_schema_defaults(
                const object& root_schema,
                const object& schema,
                const object& base_values,
                const object& override_values,
                overlay_options options)
            {
                if (override_values.get_type() == object::type::null_type
                    && options.null_policy == overlay_null_policy::ignore)
                {
                    return base_values;
                }

                if (base_values.get_type() != object::type::map_type || override_values.get_type() != object::type::map_type)
                {
                    return override_values;
                }

                map merged = base_values.as_map();
                for (const auto& [property_name, override_value] : override_values.as_map())
                {
                    if (override_value.get_type() == object::type::null_type)
                    {
                        if (options.null_policy == overlay_null_policy::ignore)
                            continue;
                        if (options.null_policy == overlay_null_policy::erase)
                        {
                            merged.erase(property_name);
                            continue;
                        }
                    }

                    std::vector<const object*> active_schemas;
                    const auto* property_schema = find_property_schema(root_schema, schema, property_name, active_schemas);
                    auto existing = merged.find(property_name);
                    if (existing != merged.end())
                    {
                        if (property_schema && existing->second.get_type() == object::type::map_type
                            && override_value.get_type() == object::type::map_type)
                        {
                            existing->second = merge_overlay_with_present_schema_defaults(
                                root_schema, *property_schema, existing->second, override_value, options);
                        }
                        else
                        {
                            existing->second = merge_overlay(existing->second, override_value, options);
                        }
                        continue;
                    }

                    if (property_schema && override_value.get_type() == object::type::map_type)
                    {
                        active_schemas.clear();
                        auto defaults = collect_schema_defaults(root_schema, *property_schema, active_schemas, true);
                        merged.emplace(
                            property_name,
                            merge_overlay_with_present_schema_defaults(
                                root_schema, *property_schema, defaults.value_or(object(map{})), override_value, options));
                        continue;
                    }

                    if (override_value.get_type() == object::type::map_type)
                        merged.emplace(property_name, merge_overlay(object(map{}), override_value, options));
                    else
                        merged.emplace(property_name, override_value);
                }

                return object(std::move(merged));
            }
        } // namespace detail

        // Extract only the values declared by schema "default" annotations.
        // The returned object is intended to be the first layer in an overlay
        // chain, not a replacement for validating the final configuration.
        [[nodiscard]] inline object schema_default_values(const object& schema)
        {
            std::vector<const object*> active_schemas;
            auto defaults = detail::collect_schema_defaults(schema, schema, active_schemas, false);
            if (defaults)
                return *defaults;
            return object(map{});
        }

        // Apply schema defaults first, then component defaults, then caller
        // overrides, and validate the result. Use this at the boundary where raw
        // JSON is still acceptable; components should usually consume a typed
        // options object after this step.
        [[nodiscard]] inline object apply_schema_overlay(
            const object& schema,
            const object& default_values,
            const object& override_values,
            overlay_options options = {})
        {
            auto effective_values = schema_default_values(schema);
            effective_values = detail::merge_overlay_with_present_schema_defaults(
                schema, schema, effective_values, default_values, options);
            effective_values = detail::merge_overlay_with_present_schema_defaults(
                schema, schema, effective_values, override_values, options);

            if (options.validate_result)
                schema::schema_validator(schema).validate_or_throw(effective_values);

            return effective_values;
        }

        // Full layering for command-line tools and tests:
        // schema defaults < component defaults < config-file values < CLI values.
        [[nodiscard]] inline object apply_schema_overlay(
            const object& schema,
            const object& default_values,
            const object& config_values,
            const object& override_values,
            overlay_options options = {})
        {
            auto effective_values = schema_default_values(schema);
            effective_values = detail::merge_overlay_with_present_schema_defaults(
                schema, schema, effective_values, default_values, options);
            effective_values = detail::merge_overlay_with_present_schema_defaults(
                schema, schema, effective_values, config_values, options);
            effective_values = detail::merge_overlay_with_present_schema_defaults(
                schema, schema, effective_values, override_values, options);

            if (options.validate_result)
                schema::schema_validator(schema).validate_or_throw(effective_values);

            return effective_values;
        }

        [[nodiscard]] inline object apply_schema_overlay(
            std::string_view schema_json,
            const object& default_values,
            const object& override_values,
            overlay_options options = {})
        {
            return apply_schema_overlay(parse(schema_json), default_values, override_values, options);
        }

        [[nodiscard]] inline object apply_schema_overlay(
            std::string_view schema_json,
            const object& default_values,
            const object& config_values,
            const object& override_values,
            overlay_options options = {})
        {
            return apply_schema_overlay(parse(schema_json), default_values, config_values, override_values, options);
        }
    } // namespace v1
} // namespace json
