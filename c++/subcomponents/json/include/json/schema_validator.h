/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <json/json_dom.h>

namespace json
{
    inline namespace v1
    {
        namespace schema
        {
            struct validation_error
            {
                std::string path;
                std::string schema_path;
                std::string message;
            };

            class validation_result
            {
            public:
                [[nodiscard]] bool valid() const noexcept { return errors_.empty(); }
                [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
                [[nodiscard]] const std::vector<validation_error>& errors() const noexcept { return errors_; }

                void add_error(
                    std::string path,
                    std::string schema_path,
                    std::string message)
                {
                    errors_.push_back({std::move(path), std::move(schema_path), std::move(message)});
                }

                void append(validation_result&& other)
                {
                    errors_.insert(
                        errors_.end(),
                        std::make_move_iterator(other.errors_.begin()),
                        std::make_move_iterator(other.errors_.end()));
                }

            private:
                std::vector<validation_error> errors_;
            };

            class validation_exception : public std::runtime_error
            {
            public:
                explicit validation_exception(validation_result result)
                    : std::runtime_error(
                          result.errors().empty() ? "JSON schema validation failed" : result.errors().front().message)
                    , result_(std::move(result))
                {
                }

                [[nodiscard]] const validation_result& result() const noexcept { return result_; }

            private:
                validation_result result_;
            };

            class schema_validator
            {
            public:
                schema_validator() = default;

                explicit schema_validator(object schema)
                    : root_schema_(std::move(schema))
                {
                }

                explicit schema_validator(std::string_view schema_json)
                    : root_schema_(parse(schema_json))
                {
                }

                void set_root_schema(object schema) { root_schema_ = std::move(schema); }

                void set_root_schema(std::string_view schema_json) { root_schema_ = parse(schema_json); }

                [[nodiscard]] validation_result validate(const object& instance) const
                {
                    validation_context context{root_schema_, {}, 0};
                    validation_result result;
                    validate_schema(root_schema_, instance, "", "", result, context);
                    return result;
                }

                void validate_or_throw(const object& instance) const
                {
                    auto result = validate(instance);
                    if (!result)
                        throw validation_exception(std::move(result));
                }

                [[nodiscard]] static validation_result validate(
                    const object& schema,
                    const object& instance)
                {
                    return schema_validator(schema).validate(instance);
                }

                [[nodiscard]] static validation_result validate_json(
                    std::string_view schema_json,
                    std::string_view instance_json)
                {
                    return schema_validator(schema_json).validate(parse(instance_json));
                }

            private:
                struct validation_context
                {
                    const object& root_schema;
                    struct active_ref
                    {
                        const object* schema = nullptr;
                        const object* instance = nullptr;
                    };

                    std::vector<active_ref> active_refs;
                    size_t evaluation_depth = 0;
                };

                object root_schema_{map{}};
                static constexpr size_t max_schema_evaluation_depth = 4096;

                struct evaluation_depth_guard
                {
                    explicit evaluation_depth_guard(validation_context& validation_context)
                        : context(validation_context)
                    {
                        ++context.evaluation_depth;
                    }

                    ~evaluation_depth_guard() { --context.evaluation_depth; }

                    validation_context& context;
                };

                [[nodiscard]] static std::string path_or_root(const std::string& path)
                {
                    return path.empty() ? "/" : path;
                }

                [[nodiscard]] static std::string escape_pointer_token(std::string_view token)
                {
                    std::string escaped;
                    escaped.reserve(token.size());
                    for (const char ch : token)
                    {
                        if (ch == '~')
                            escaped += "~0";
                        else if (ch == '/')
                            escaped += "~1";
                        else
                            escaped += ch;
                    }
                    return escaped;
                }

                [[nodiscard]] static std::string append_path(
                    const std::string& path,
                    std::string_view token)
                {
                    return path + "/" + escape_pointer_token(token);
                }

                [[nodiscard]] static std::string append_schema_path(
                    const std::string& path,
                    std::string_view token)
                {
                    return append_path(path, token);
                }

                [[nodiscard]] static const object* map_find(
                    const map& values,
                    std::string_view key)
                {
                    const auto it = values.find(std::string(key));
                    if (it == values.end())
                        return nullptr;
                    return &it->second;
                }

                [[nodiscard]] static std::optional<std::string> string_value(const object& value)
                {
                    if (value.get_type() != object::type::string_type)
                        return std::nullopt;
                    return value.get<std::string>();
                }

                [[nodiscard]] static std::optional<double> number_value(const object& value)
                {
                    if (value.get_type() != object::type::number_type)
                        return std::nullopt;
                    return value.get<number>().as_double();
                }

                [[nodiscard]] static std::optional<uint64_t> uint_value(const object& value)
                {
                    if (value.get_type() != object::type::number_type)
                        return std::nullopt;
                    try
                    {
                        return value.get<number>().as_uint64();
                    }
                    catch (const std::out_of_range&)
                    {
                        return std::nullopt;
                    }
                }

                [[nodiscard]] static bool is_integer(const object& instance)
                {
                    if (instance.get_type() != object::type::number_type)
                        return false;

                    const auto value = instance.get<number>();
                    switch (value.get_type())
                    {
                    case number::type::signed_integer:
                    case number::type::unsigned_integer:
                        return true;
                    case number::type::floating:
                        break;
                    }

                    const double floating = value.as_double();
                    return std::isfinite(floating) && std::floor(floating) == floating;
                }

                [[nodiscard]] static const char* type_name(const object& value)
                {
                    switch (value.get_type())
                    {
                    case object::type::string_type:
                        return "string";
                    case object::type::number_type:
                        return is_integer(value) ? "integer" : "number";
                    case object::type::bool_type:
                        return "boolean";
                    case object::type::null_type:
                        return "null";
                    case object::type::array_type:
                        return "array";
                    case object::type::map_type:
                        return "object";
                    }
                    return "unknown";
                }

                [[nodiscard]] static bool number_equal(
                    const number& lhs,
                    const number& rhs)
                {
                    if (lhs.get_type() == rhs.get_type())
                        return lhs == rhs;

                    if (lhs.get_type() == number::type::signed_integer && rhs.get_type() == number::type::unsigned_integer)
                    {
                        const auto lhs_value = lhs.as_int64();
                        return lhs_value >= 0 && static_cast<uint64_t>(lhs_value) == rhs.as_uint64();
                    }

                    if (lhs.get_type() == number::type::unsigned_integer && rhs.get_type() == number::type::signed_integer)
                    {
                        const auto rhs_value = rhs.as_int64();
                        return rhs_value >= 0 && lhs.as_uint64() == static_cast<uint64_t>(rhs_value);
                    }

                    return lhs.as_double() == rhs.as_double();
                }

                [[nodiscard]] static bool schema_equal(
                    const object& lhs,
                    const object& rhs)
                {
                    if (lhs.get_type() != rhs.get_type())
                        return false;

                    if (lhs.get_type() == object::type::number_type)
                        return number_equal(lhs.get<number>(), rhs.get<number>());

                    if (lhs.get_type() == object::type::array_type)
                    {
                        const auto& lhs_values = lhs.as_array();
                        const auto& rhs_values = rhs.as_array();
                        if (lhs_values.size() != rhs_values.size())
                            return false;
                        for (size_t i = 0; i < lhs_values.size(); ++i)
                        {
                            if (!schema_equal(lhs_values[i], rhs_values[i]))
                                return false;
                        }
                        return true;
                    }

                    if (lhs.get_type() == object::type::map_type)
                    {
                        const auto& lhs_values = lhs.as_map();
                        const auto& rhs_values = rhs.as_map();
                        if (lhs_values.size() != rhs_values.size())
                            return false;
                        for (const auto& [name, lhs_value] : lhs_values)
                        {
                            const auto* rhs_value = map_find(rhs_values, name);
                            if (!rhs_value || !schema_equal(lhs_value, *rhs_value))
                                return false;
                        }
                        return true;
                    }

                    return lhs == rhs;
                }

                [[nodiscard]] static bool has_type(
                    const object& instance,
                    const std::string& expected)
                {
                    if (expected == "string")
                        return instance.get_type() == object::type::string_type;
                    if (expected == "number")
                        return instance.get_type() == object::type::number_type;
                    if (expected == "integer")
                        return is_integer(instance);
                    if (expected == "boolean")
                        return instance.get_type() == object::type::bool_type;
                    if (expected == "null")
                        return instance.get_type() == object::type::null_type;
                    if (expected == "array")
                        return instance.get_type() == object::type::array_type;
                    if (expected == "object")
                        return instance.get_type() == object::type::map_type;
                    return false;
                }

                static void add_error(
                    validation_result& result,
                    const std::string& path,
                    const std::string& schema_path,
                    std::string message)
                {
                    result.add_error(path_or_root(path), path_or_root(schema_path), std::move(message));
                }

                static void validate_schema(
                    const object& schema,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    if (context.evaluation_depth >= max_schema_evaluation_depth)
                    {
                        add_error(result, path, schema_path, "maximum schema evaluation depth exceeded");
                        return;
                    }
                    evaluation_depth_guard depth_guard(context);

                    if (schema.get_type() == object::type::bool_type)
                    {
                        if (!schema.get<bool>())
                            add_error(result, path, schema_path, "false schema rejected value");
                        return;
                    }

                    if (schema.get_type() != object::type::map_type)
                    {
                        add_error(result, path, schema_path, "schema must be an object or boolean");
                        return;
                    }

                    const auto& schema_map = schema.as_map();

                    // Canopy's validator does not support regular-expression
                    // keywords. The IDL-driven schema generator never emits
                    // them; a hand-written schema that uses them needs to be
                    // surfaced rather than silently accepted, otherwise a
                    // pattern constraint declared by a downstream tool would
                    // be quietly dropped.
                    if (map_find(schema_map, "pattern"))
                    {
                        add_error(
                            result,
                            path,
                            append_schema_path(schema_path, "pattern"),
                            "the `pattern` keyword is not supported by this schema validator");
                        return;
                    }
                    if (map_find(schema_map, "patternProperties"))
                    {
                        add_error(
                            result,
                            path,
                            append_schema_path(schema_path, "patternProperties"),
                            "the `patternProperties` keyword is not supported by this schema validator");
                        return;
                    }

                    if (const auto* ref = map_find(schema_map, "$ref"))
                    {
                        validate_ref(*ref, instance, path, append_schema_path(schema_path, "$ref"), result, context);
                        return;
                    }

                    validate_type(schema_map, instance, path, schema_path, result);
                    validate_const_and_enum(schema_map, instance, path, schema_path, result);
                    validate_numeric(schema_map, instance, path, schema_path, result);
                    validate_string(schema_map, instance, path, schema_path, result);
                    validate_array(schema_map, instance, path, schema_path, result, context);
                    validate_object(schema_map, instance, path, schema_path, result, context);
                    validate_combinators(schema_map, instance, path, schema_path, result, context);
                    validate_conditional(schema_map, instance, path, schema_path, result, context);
                }

                static void validate_type(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result)
                {
                    const auto* type_schema = map_find(schema_map, "type");
                    if (!type_schema)
                        return;

                    if (const auto expected = string_value(*type_schema))
                    {
                        if (!has_type(instance, *expected))
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "type"),
                                fmt::format("expected type {}, got {}", *expected, type_name(instance)));
                        }
                        return;
                    }

                    if (type_schema->get_type() == object::type::array_type)
                    {
                        for (const auto& entry : type_schema->as_array())
                        {
                            const auto expected = string_value(entry);
                            if (expected && has_type(instance, *expected))
                                return;
                        }
                        add_error(
                            result,
                            path,
                            append_schema_path(schema_path, "type"),
                            fmt::format("type {} is not in allowed type list", type_name(instance)));
                        return;
                    }

                    add_error(
                        result, path, append_schema_path(schema_path, "type"), "type keyword must be a string or array");
                }

                static void validate_const_and_enum(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result)
                {
                    if (const auto* const_schema = map_find(schema_map, "const"))
                    {
                        if (!schema_equal(*const_schema, instance))
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "const"),
                                "value does not match const schema value");
                        }
                    }

                    const auto* enum_schema = map_find(schema_map, "enum");
                    if (!enum_schema)
                        return;

                    if (enum_schema->get_type() != object::type::array_type)
                    {
                        add_error(result, path, append_schema_path(schema_path, "enum"), "enum keyword must be an array");
                        return;
                    }

                    for (const auto& candidate : enum_schema->as_array())
                    {
                        if (schema_equal(candidate, instance))
                            return;
                    }

                    add_error(result, path, append_schema_path(schema_path, "enum"), "value is not one of the enum values");
                }

                static void validate_numeric(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result)
                {
                    if (instance.get_type() != object::type::number_type)
                        return;

                    const double value = instance.get<number>().as_double();

                    if (const auto* minimum = map_find(schema_map, "minimum"))
                    {
                        if (const auto bound = number_value(*minimum); bound && value < *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "minimum"),
                                fmt::format("number must be >= {}", *bound));
                        }
                    }

                    if (const auto* maximum = map_find(schema_map, "maximum"))
                    {
                        if (const auto bound = number_value(*maximum); bound && value > *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "maximum"),
                                fmt::format("number must be <= {}", *bound));
                        }
                    }

                    validate_exclusive_numeric_bound(
                        schema_map, "exclusiveMinimum", true, value, path, schema_path, result);
                    validate_exclusive_numeric_bound(
                        schema_map, "exclusiveMaximum", false, value, path, schema_path, result);

                    if (const auto* multiple_of = map_find(schema_map, "multipleOf"))
                    {
                        if (const auto divisor = number_value(*multiple_of); divisor && *divisor > 0.0)
                        {
                            const double quotient = value / *divisor;
                            const double nearest = std::round(quotient);
                            if (std::abs(quotient - nearest) > 1e-12)
                            {
                                add_error(
                                    result,
                                    path,
                                    append_schema_path(schema_path, "multipleOf"),
                                    fmt::format("number must be a multiple of {}", *divisor));
                            }
                        }
                    }
                }

                static void validate_exclusive_numeric_bound(
                    const map& schema_map,
                    std::string_view keyword,
                    bool lower_bound,
                    double value,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result)
                {
                    const auto* bound_schema = map_find(schema_map, keyword);
                    if (!bound_schema)
                        return;

                    std::optional<double> bound;
                    if (bound_schema->get_type() == object::type::bool_type)
                    {
                        if (!bound_schema->get<bool>())
                            return;
                        const auto* inclusive = map_find(schema_map, lower_bound ? "minimum" : "maximum");
                        if (inclusive)
                            bound = number_value(*inclusive);
                    }
                    else
                    {
                        bound = number_value(*bound_schema);
                    }

                    if (!bound)
                        return;

                    if (lower_bound && value <= *bound)
                    {
                        add_error(
                            result, path, append_schema_path(schema_path, keyword), fmt::format("number must be > {}", *bound));
                    }
                    else if (!lower_bound && value >= *bound)
                    {
                        add_error(
                            result, path, append_schema_path(schema_path, keyword), fmt::format("number must be < {}", *bound));
                    }
                }

                static void validate_string(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result)
                {
                    if (instance.get_type() != object::type::string_type)
                        return;

                    const auto value = instance.get<std::string>();
                    if (const auto* min_length = map_find(schema_map, "minLength"))
                    {
                        if (const auto bound = uint_value(*min_length); bound && value.size() < *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "minLength"),
                                fmt::format("string length must be >= {}", *bound));
                        }
                    }

                    if (const auto* max_length = map_find(schema_map, "maxLength"))
                    {
                        if (const auto bound = uint_value(*max_length); bound && value.size() > *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "maxLength"),
                                fmt::format("string length must be <= {}", *bound));
                        }
                    }
                }

                static void validate_array(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    if (instance.get_type() != object::type::array_type)
                        return;

                    const auto& values = instance.as_array();
                    if (const auto* min_items = map_find(schema_map, "minItems"))
                    {
                        if (const auto bound = uint_value(*min_items); bound && values.size() < *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "minItems"),
                                fmt::format("array size must be >= {}", *bound));
                        }
                    }

                    if (const auto* max_items = map_find(schema_map, "maxItems"))
                    {
                        if (const auto bound = uint_value(*max_items); bound && values.size() > *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "maxItems"),
                                fmt::format("array size must be <= {}", *bound));
                        }
                    }

                    if (const auto* unique_items = map_find(schema_map, "uniqueItems");
                        unique_items && unique_items->get_type() == object::type::bool_type && unique_items->get<bool>())
                    {
                        for (size_t i = 0; i < values.size(); ++i)
                        {
                            for (size_t j = i + 1; j < values.size(); ++j)
                            {
                                if (schema_equal(values[i], values[j]))
                                {
                                    add_error(
                                        result,
                                        append_path(path, std::to_string(j)),
                                        append_schema_path(schema_path, "uniqueItems"),
                                        "array items must be unique");
                                    i = values.size();
                                    break;
                                }
                            }
                        }
                    }

                    const auto* items = map_find(schema_map, "items");
                    if (!items)
                        return;

                    if (items->get_type() == object::type::map_type || items->get_type() == object::type::bool_type)
                    {
                        for (size_t i = 0; i < values.size(); ++i)
                        {
                            validate_schema(
                                *items,
                                values[i],
                                append_path(path, std::to_string(i)),
                                append_schema_path(schema_path, "items"),
                                result,
                                context);
                        }
                        return;
                    }

                    if (items->get_type() == object::type::array_type)
                    {
                        const auto& tuple_schemas = items->as_array();
                        for (size_t i = 0; i < values.size() && i < tuple_schemas.size(); ++i)
                        {
                            validate_schema(
                                tuple_schemas[i],
                                values[i],
                                append_path(path, std::to_string(i)),
                                append_path(append_schema_path(schema_path, "items"), std::to_string(i)),
                                result,
                                context);
                        }

                        if (values.size() > tuple_schemas.size())
                        {
                            const auto* additional_items = map_find(schema_map, "additionalItems");
                            if (additional_items)
                            {
                                for (size_t i = tuple_schemas.size(); i < values.size(); ++i)
                                {
                                    validate_schema(
                                        *additional_items,
                                        values[i],
                                        append_path(path, std::to_string(i)),
                                        append_schema_path(schema_path, "additionalItems"),
                                        result,
                                        context);
                                }
                            }
                        }
                    }
                }

                static void validate_object(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    if (instance.get_type() != object::type::map_type)
                        return;

                    const auto& values = instance.as_map();
                    if (const auto* min_properties = map_find(schema_map, "minProperties"))
                    {
                        if (const auto bound = uint_value(*min_properties); bound && values.size() < *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "minProperties"),
                                fmt::format("object property count must be >= {}", *bound));
                        }
                    }

                    if (const auto* max_properties = map_find(schema_map, "maxProperties"))
                    {
                        if (const auto bound = uint_value(*max_properties); bound && values.size() > *bound)
                        {
                            add_error(
                                result,
                                path,
                                append_schema_path(schema_path, "maxProperties"),
                                fmt::format("object property count must be <= {}", *bound));
                        }
                    }

                    validate_required(schema_map, values, path, schema_path, result);
                    validate_properties(schema_map, values, path, schema_path, result, context);
                }

                static void validate_required(
                    const map& schema_map,
                    const map& values,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result)
                {
                    const auto* required = map_find(schema_map, "required");
                    if (!required)
                        return;

                    if (required->get_type() != object::type::array_type)
                    {
                        add_error(result, path, append_schema_path(schema_path, "required"), "required must be an array");
                        return;
                    }

                    for (size_t i = 0; i < required->as_array().size(); ++i)
                    {
                        const auto required_name = string_value(required->as_array()[i]);
                        if (!required_name)
                        {
                            add_error(
                                result,
                                path,
                                append_path(append_schema_path(schema_path, "required"), std::to_string(i)),
                                "required entry must be a string");
                            continue;
                        }

                        if (!map_find(values, *required_name))
                        {
                            add_error(
                                result,
                                append_path(path, *required_name),
                                append_path(append_schema_path(schema_path, "required"), std::to_string(i)),
                                fmt::format("required property '{}' is missing", *required_name));
                        }
                    }
                }

                static void validate_properties(
                    const map& schema_map,
                    const map& values,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* properties = map_find(schema_map, "properties");
                    const map* property_schemas = nullptr;
                    if (properties)
                    {
                        if (properties->get_type() != object::type::map_type)
                        {
                            add_error(
                                result, path, append_schema_path(schema_path, "properties"), "properties must be an object");
                        }
                        else
                        {
                            property_schemas = &properties->as_map();
                        }
                    }

                    std::set<std::string> matched_properties;
                    if (property_schemas)
                    {
                        for (const auto& [name, property_schema] : *property_schemas)
                        {
                            if (const auto* value = map_find(values, name))
                            {
                                matched_properties.insert(name);
                                validate_schema(
                                    property_schema,
                                    *value,
                                    append_path(path, name),
                                    append_path(append_schema_path(schema_path, "properties"), name),
                                    result,
                                    context);
                            }
                        }
                    }

                    validate_additional_properties(
                        schema_map, values, matched_properties, path, schema_path, result, context);
                    validate_property_names(schema_map, values, path, schema_path, result, context);
                }

                static void validate_additional_properties(
                    const map& schema_map,
                    const map& values,
                    const std::set<std::string>& matched_properties,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* additional = map_find(schema_map, "additionalProperties");
                    if (!additional)
                        return;

                    for (const auto& [name, value] : values)
                    {
                        if (matched_properties.find(name) != matched_properties.end())
                            continue;

                        if (additional->get_type() == object::type::bool_type && !additional->get<bool>())
                        {
                            add_error(
                                result,
                                append_path(path, name),
                                append_schema_path(schema_path, "additionalProperties"),
                                fmt::format("additional property '{}' is not allowed", name));
                        }
                        else
                        {
                            validate_schema(
                                *additional,
                                value,
                                append_path(path, name),
                                append_schema_path(schema_path, "additionalProperties"),
                                result,
                                context);
                        }
                    }
                }

                static void validate_property_names(
                    const map& schema_map,
                    const map& values,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* property_names = map_find(schema_map, "propertyNames");
                    if (!property_names)
                        return;

                    for (const auto& [name, ignored] : values)
                    {
                        (void)ignored;
                        const object property_name(name);
                        validate_schema(
                            *property_names,
                            property_name,
                            append_path(path, name),
                            append_schema_path(schema_path, "propertyNames"),
                            result,
                            context);
                    }
                }

                static void validate_combinators(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    validate_all_of(schema_map, instance, path, schema_path, result, context);
                    validate_any_of(schema_map, instance, path, schema_path, result, context);
                    validate_one_of(schema_map, instance, path, schema_path, result, context);
                    validate_not(schema_map, instance, path, schema_path, result, context);
                }

                static void validate_all_of(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* all_of = map_find(schema_map, "allOf");
                    if (!all_of)
                        return;

                    if (all_of->get_type() != object::type::array_type)
                    {
                        add_error(result, path, append_schema_path(schema_path, "allOf"), "allOf must be an array");
                        return;
                    }

                    for (size_t i = 0; i < all_of->as_array().size(); ++i)
                    {
                        validate_schema(
                            all_of->as_array()[i],
                            instance,
                            path,
                            append_path(append_schema_path(schema_path, "allOf"), std::to_string(i)),
                            result,
                            context);
                    }
                }

                static void validate_any_of(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* any_of = map_find(schema_map, "anyOf");
                    if (!any_of)
                        return;

                    if (any_of->get_type() != object::type::array_type)
                    {
                        add_error(result, path, append_schema_path(schema_path, "anyOf"), "anyOf must be an array");
                        return;
                    }

                    for (size_t i = 0; i < any_of->as_array().size(); ++i)
                    {
                        validation_result branch;
                        validate_schema(
                            any_of->as_array()[i],
                            instance,
                            path,
                            append_path(append_schema_path(schema_path, "anyOf"), std::to_string(i)),
                            branch,
                            context);
                        if (branch.valid())
                            return;
                    }

                    add_error(
                        result, path, append_schema_path(schema_path, "anyOf"), "value does not match any anyOf schema");
                }

                static void validate_one_of(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* one_of = map_find(schema_map, "oneOf");
                    if (!one_of)
                        return;

                    if (one_of->get_type() != object::type::array_type)
                    {
                        add_error(result, path, append_schema_path(schema_path, "oneOf"), "oneOf must be an array");
                        return;
                    }

                    size_t matches = 0;
                    for (size_t i = 0; i < one_of->as_array().size(); ++i)
                    {
                        validation_result branch;
                        validate_schema(
                            one_of->as_array()[i],
                            instance,
                            path,
                            append_path(append_schema_path(schema_path, "oneOf"), std::to_string(i)),
                            branch,
                            context);
                        if (branch.valid())
                            ++matches;
                    }

                    if (matches != 1)
                    {
                        add_error(
                            result,
                            path,
                            append_schema_path(schema_path, "oneOf"),
                            fmt::format("value must match exactly one oneOf schema, matched {}", matches));
                    }
                }

                static void validate_not(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* not_schema = map_find(schema_map, "not");
                    if (!not_schema)
                        return;

                    validation_result branch;
                    validate_schema(*not_schema, instance, path, append_schema_path(schema_path, "not"), branch, context);
                    if (branch.valid())
                        add_error(result, path, append_schema_path(schema_path, "not"), "value matched not schema");
                }

                static void validate_conditional(
                    const map& schema_map,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto* if_schema = map_find(schema_map, "if");
                    if (!if_schema)
                        return;

                    validation_result condition;
                    validate_schema(*if_schema, instance, path, append_schema_path(schema_path, "if"), condition, context);
                    const auto* branch_schema
                        = condition.valid() ? map_find(schema_map, "then") : map_find(schema_map, "else");
                    if (branch_schema)
                    {
                        validate_schema(
                            *branch_schema,
                            instance,
                            path,
                            append_schema_path(schema_path, condition.valid() ? "then" : "else"),
                            result,
                            context);
                    }
                }

                static void validate_ref(
                    const object& ref_schema,
                    const object& instance,
                    const std::string& path,
                    const std::string& schema_path,
                    validation_result& result,
                    validation_context& context)
                {
                    const auto ref = string_value(ref_schema);
                    if (!ref)
                    {
                        add_error(result, path, schema_path, "$ref must be a string");
                        return;
                    }

                    if (ref->empty() || (*ref)[0] != '#')
                    {
                        add_error(result, path, schema_path, "only local JSON pointer $ref values are supported");
                        return;
                    }

                    const object* target = resolve_pointer(context.root_schema, ref->substr(1));
                    if (!target)
                    {
                        add_error(result, path, schema_path, fmt::format("unresolved $ref '{}'", *ref));
                        return;
                    }

                    const validation_context::active_ref active{target, &instance};
                    const auto already_active = std::find_if(
                        context.active_refs.begin(),
                        context.active_refs.end(),
                        [&](const auto& item)
                        { return item.schema == active.schema && item.instance == active.instance; });
                    if (already_active != context.active_refs.end())
                    {
                        return;
                    }

                    context.active_refs.push_back(active);
                    validate_schema(*target, instance, path, *ref, result, context);
                    context.active_refs.pop_back();
                }

                [[nodiscard]] static std::string unescape_pointer_token(std::string_view token)
                {
                    std::string unescaped;
                    unescaped.reserve(token.size());
                    for (size_t i = 0; i < token.size(); ++i)
                    {
                        if (token[i] == '~' && i + 1 < token.size())
                        {
                            if (token[i + 1] == '0')
                            {
                                unescaped += '~';
                                ++i;
                                continue;
                            }
                            if (token[i + 1] == '1')
                            {
                                unescaped += '/';
                                ++i;
                                continue;
                            }
                        }
                        unescaped += token[i];
                    }
                    return unescaped;
                }

                [[nodiscard]] static const object* resolve_pointer(
                    const object& root,
                    std::string_view pointer)
                {
                    if (pointer.empty())
                        return &root;
                    if (pointer.front() != '/')
                        return nullptr;

                    const object* current = &root;
                    size_t token_start = 1;
                    while (token_start <= pointer.size())
                    {
                        const size_t token_end = pointer.find('/', token_start);
                        const auto raw_token = pointer.substr(
                            token_start,
                            token_end == std::string_view::npos ? std::string_view::npos : token_end - token_start);
                        const auto token = unescape_pointer_token(raw_token);

                        if (current->get_type() == object::type::map_type)
                        {
                            current = map_find(current->as_map(), token);
                        }
                        else if (current->get_type() == object::type::array_type)
                        {
                            const auto& values = current->as_array();
                            uint64_t index = 0;
                            try
                            {
                                index = number::parse(token).as_uint64();
                            }
                            catch (const std::exception&)
                            {
                                return nullptr;
                            }
                            if (index >= values.size())
                                return nullptr;
                            current = &values[static_cast<size_t>(index)];
                        }
                        else
                        {
                            return nullptr;
                        }

                        if (!current || token_end == std::string_view::npos)
                            return current;
                        token_start = token_end + 1;
                    }

                    return current;
                }
            };
        } // namespace schema
    } // namespace v1
} // namespace json
