/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <json/schema_validator.h>

// RPC headers
#include <rpc/rpc.h>

// Other headers
#include <example/example.h>
#include <example_shared/example_shared.h>
#include <example_shared/example_shared_schema.h>
#include <gtest/gtest.h>
#include <schema_cycle/schema_cycle_schema.h>

namespace
{
    const json::v1::object* find_member(
        const json::v1::object& value,
        const std::string& name)
    {
        if (value.get_type() != json::v1::object::type::map_type)
            return nullptr;

        const auto& values = value.as_map();
        const auto it = values.find(name);
        if (it == values.end())
            return nullptr;
        return &it->second;
    }

    bool has_member(
        const json::v1::object& value,
        const std::string& name)
    {
        return find_member(value, name) != nullptr;
    }

    bool is_schema_object(const json::v1::object& schema)
    {
        const auto* type = find_member(schema, "type");
        return type && type->get_type() == json::v1::object::type::string_type && type->get<std::string>() == "object";
    }

    bool string_member_contains(
        const json::v1::object& value,
        const std::string& name,
        const std::string& needle)
    {
        const auto* member = find_member(value, name);
        return member && member->get_type() == json::v1::object::type::string_type
               && member->get<std::string>().find(needle) != std::string::npos;
    }

    bool has_non_empty_properties(const json::v1::object& schema)
    {
        const auto* properties = find_member(schema, "properties");
        return properties && properties->get_type() == json::v1::object::type::map_type && !properties->as_map().empty();
    }

    bool required_contains(
        const json::v1::object& schema,
        const std::string& name)
    {
        const auto* required = find_member(schema, "required");
        if (!required || required->get_type() != json::v1::object::type::array_type)
            return false;

        const auto& required_values = required->as_array();
        return std::any_of(
            required_values.begin(),
            required_values.end(),
            [&](const auto& value)
            {
                return value.get_type() == json::v1::object::type::string_type && value.template get<std::string>() == name;
            });
    }

    bool has_schema_type(
        const json::v1::object& schema,
        const std::string& type)
    {
        const auto* type_member = find_member(schema, "type");
        return type_member && type_member->get_type() == json::v1::object::type::string_type
               && type_member->get<std::string>() == type;
    }

    bool one_of_contains_schema_type(
        const json::v1::object& schema,
        const std::string& type)
    {
        const auto* one_of = find_member(schema, "oneOf");
        if (!one_of || one_of->get_type() != json::v1::object::type::array_type)
            return false;

        const auto& alternatives = one_of->as_array();
        return std::any_of(
            alternatives.begin(),
            alternatives.end(),
            [&](const auto& alternative)
            { return alternative.get_type() == json::v1::object::type::map_type && has_schema_type(alternative, type); });
    }

    bool rpc_variant_one_of_case_has_type(
        const json::v1::object& schema,
        const std::string& case_name,
        const std::string& type)
    {
        const auto* one_of = find_member(schema, "oneOf");
        if (!one_of || one_of->get_type() != json::v1::object::type::array_type)
            return false;

        const auto& alternatives = one_of->as_array();
        return std::any_of(
            alternatives.begin(),
            alternatives.end(),
            [&](const auto& alternative)
            {
                if (alternative.get_type() != json::v1::object::type::map_type)
                    return false;

                const auto* properties = find_member(alternative, "properties");
                if (!properties || properties->get_type() != json::v1::object::type::map_type)
                    return false;

                const auto* case_schema = find_member(*properties, case_name);
                return case_schema && has_schema_type(*case_schema, type) && required_contains(alternative, case_name);
            });
    }

    void expect_valid_schema_instance(
        const json::v1::object& schema,
        const json::v1::object& instance)
    {
        const auto result = json::v1::schema::schema_validator(schema).validate(instance);
        if (!result)
        {
            FAIL() << (result.errors().empty() ? std::string("schema validation failed") : result.errors().front().message);
        }
    }

}

// Test using inproc_setup to call standard_tests for schema validation
TEST(
    JSONSchemaValidationTest,
    InprocSetupStandardTests)
{
    // Serialize a call to call_host_create_enclave_and_throw_away with parameter false
    std::vector<char> buffer;
    auto err
        = yyy::i_example::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::call_host_create_enclave_and_throw_away(
            false, buffer, rpc::encoding::yas_json);

    ASSERT_EQ(err, 0) << "Serialization should succeed";
    ASSERT_FALSE(buffer.empty()) << "Buffer should not be empty after serialization";

    // Get function info and find the matching function
    auto fi = yyy::i_example::get_function_info();
    ASSERT_FALSE(fi.empty()) << "Function info should not be empty";

    // Find the call_host_create_enclave_and_throw_away function in the real function info
    auto it = std::find_if(
        fi.begin(), fi.end(), [](const auto& func) { return func.name == "call_host_create_enclave_and_throw_away"; });

    ASSERT_NE(it, fi.end()) << "Should find call_host_create_enclave_and_throw_away function";

    // Convert buffer to string and parse as JSON
    std::string buffer_str(buffer.begin(), buffer.end());
    EXPECT_FALSE(buffer_str.empty()) << "Serialized buffer should contain data";

    // Parse the serialized data as JSON
    json::v1::object payload_json;
    ASSERT_NO_THROW(payload_json = json::v1::parse(buffer_str)) << "Buffer should contain valid JSON";

    // Parse the schema from the real function info
    const auto& schema_str = it->in_json_schema;
    EXPECT_FALSE(schema_str.empty()) << "Schema should not be empty";

    json::v1::object schema_json;
    ASSERT_NO_THROW(schema_json = json::v1::parse(schema_str)) << "Schema should be valid JSON";

    const auto validation_result = json::v1::schema::schema_validator(schema_json).validate(payload_json);
    if (!validation_result)
    {
        FAIL() << "Schema validation failed: " << validation_result.errors().front().message
               << "\nPayload: " << buffer_str << "\nSchema: " << schema_str;
    }

    // Additional checks for the specific function
    const auto* run_standard_tests = find_member(payload_json, "run_standard_tests");
    ASSERT_NE(run_standard_tests, nullptr) << "Payload should contain run_standard_tests parameter";
    EXPECT_EQ(run_standard_tests->get_type(), json::v1::object::type::bool_type)
        << "run_standard_tests should be a boolean";
    EXPECT_FALSE(run_standard_tests->get<bool>()) << "run_standard_tests should be false as passed to serializer";

    SUCCEED() << "Schema validation test completed successfully";
}

// Test that input and output schemas are properly separated
TEST(
    JSONSchemaValidationTest,
    InputOutputSchemaSeparation)
{
    // Get function info
    auto fi = yyy::i_example::get_function_info();
    ASSERT_FALSE(fi.empty()) << "Function info should not be empty";

    // Find a function with both input and output parameters
    auto it = std::find_if(fi.begin(), fi.end(), [](const auto& func) { return func.name == "call_host_create_enclave"; });

    ASSERT_NE(it, fi.end()) << "Should find call_host_create_enclave function";

    // Verify input schema exists and is valid
    EXPECT_FALSE(it->in_json_schema.empty()) << "Input schema should not be empty";

    json::v1::object in_schema_json;
    ASSERT_NO_THROW(in_schema_json = json::v1::parse(it->in_json_schema)) << "Input schema should be valid JSON";

    EXPECT_TRUE(is_schema_object(in_schema_json)) << "Input schema should be object type";
    EXPECT_TRUE(has_member(in_schema_json, "description")) << "Input schema should have description";
    EXPECT_TRUE(string_member_contains(in_schema_json, "description", "Input parameters"))
        << "Input schema description should indicate input parameters";

    // Verify output schema exists and is valid
    EXPECT_FALSE(it->out_json_schema.empty()) << "Output schema should not be empty";

    json::v1::object out_schema_json;
    ASSERT_NO_THROW(out_schema_json = json::v1::parse(it->out_json_schema)) << "Output schema should be valid JSON";

    EXPECT_TRUE(is_schema_object(out_schema_json)) << "Output schema should be object type";
    EXPECT_TRUE(has_member(out_schema_json, "description")) << "Output schema should have description";
    EXPECT_TRUE(string_member_contains(out_schema_json, "description", "Output parameters"))
        << "Output schema description should indicate output parameters";

    // Test that schemas are different (since this function has both input and output params)
    EXPECT_NE(it->in_json_schema, it->out_json_schema) << "Input and output schemas should be different";
}

// Test that functions with only input parameters have empty output schema
TEST(
    JSONSchemaValidationTest,
    InputOnlyFunctionSchemas)
{
    // Get function info
    auto fi = yyy::i_example::get_function_info();
    ASSERT_FALSE(fi.empty()) << "Function info should not be empty";

    // Find a function with only input parameters (no [out] attributes)
    auto it = std::find_if(
        fi.begin(), fi.end(), [](const auto& func) { return func.name == "call_host_create_enclave_and_throw_away"; });

    ASSERT_NE(it, fi.end()) << "Should find call_host_create_enclave_and_throw_away function";

    // Verify input schema exists
    EXPECT_FALSE(it->in_json_schema.empty()) << "Input schema should not be empty for input-only function";

    // Verify output schema indicates no output parameters
    json::v1::object out_schema_json;
    ASSERT_NO_THROW(out_schema_json = json::v1::parse(it->out_json_schema)) << "Output schema should be valid JSON";

    // For functions with no output parameters, the schema should still be valid but indicate no properties
    EXPECT_TRUE(is_schema_object(out_schema_json)) << "Output schema should be object type";
    EXPECT_FALSE(has_non_empty_properties(out_schema_json))
        << "Output schema should not have properties for input-only function";
}

TEST(
    JSONSchemaValidationTest,
    OptionalVariantAndJsonObjectSchemaMappings)
{
    const auto fi = xxx::i_foo::get_function_info();
    auto it = std::find_if(
        fi.begin(), fi.end(), [](const auto& func) { return func.name == "exchange_optional_variant_json"; });
    ASSERT_NE(it, fi.end());

    json::v1::object schema_json;
    ASSERT_NO_THROW(schema_json = json::v1::parse(it->in_json_schema));

    const auto* properties = find_member(schema_json, "properties");
    ASSERT_NE(properties, nullptr);
    ASSERT_EQ(properties->get_type(), json::v1::object::type::map_type);

    const auto* optional_int = find_member(*properties, "optional_int");
    ASSERT_NE(optional_int, nullptr);
    EXPECT_TRUE(one_of_contains_schema_type(*optional_int, "integer"));
    EXPECT_TRUE(one_of_contains_schema_type(*optional_int, "null"));
    EXPECT_FALSE(required_contains(schema_json, "optional_int"));

    const auto* variant_value = find_member(*properties, "variant_value");
    ASSERT_NE(variant_value, nullptr);
    const auto* one_of = find_member(*variant_value, "oneOf");
    ASSERT_NE(one_of, nullptr);
    ASSERT_EQ(one_of->get_type(), json::v1::object::type::array_type);
    ASSERT_EQ(one_of->get<json::v1::array>().size(), size_t{2});
    EXPECT_TRUE(rpc_variant_one_of_case_has_type(*variant_value, "int32", "integer"));
    EXPECT_TRUE(rpc_variant_one_of_case_has_type(*variant_value, "string", "string"));
    EXPECT_TRUE(required_contains(schema_json, "variant_value"));

    const auto* json_value = find_member(*properties, "json_value");
    ASSERT_NE(json_value, nullptr);
    EXPECT_FALSE(has_member(*json_value, "type"));
    EXPECT_TRUE(required_contains(schema_json, "json_value"));

    const auto* optional_json_value = find_member(*properties, "optional_json_value");
    ASSERT_NE(optional_json_value, nullptr);
    EXPECT_FALSE(has_member(*optional_json_value, "type"));
    EXPECT_FALSE(required_contains(schema_json, "optional_json_value"));

    json::v1::object out_schema_json;
    ASSERT_NO_THROW(out_schema_json = json::v1::parse(it->out_json_schema));

    const auto* out_properties = find_member(out_schema_json, "properties");
    ASSERT_NE(out_properties, nullptr);
    ASSERT_EQ(out_properties->get_type(), json::v1::object::type::map_type);

    const auto* optional_int_out = find_member(*out_properties, "optional_int_out");
    ASSERT_NE(optional_int_out, nullptr);
    EXPECT_TRUE(one_of_contains_schema_type(*optional_int_out, "integer"));
    EXPECT_TRUE(one_of_contains_schema_type(*optional_int_out, "null"));
    EXPECT_FALSE(required_contains(out_schema_json, "optional_int_out"));

    const auto* variant_value_out = find_member(*out_properties, "variant_value_out");
    ASSERT_NE(variant_value_out, nullptr);
    const auto* out_one_of = find_member(*variant_value_out, "oneOf");
    ASSERT_NE(out_one_of, nullptr);
    ASSERT_EQ(out_one_of->get_type(), json::v1::object::type::array_type);
    ASSERT_EQ(out_one_of->get<json::v1::array>().size(), size_t{2});
    EXPECT_TRUE(rpc_variant_one_of_case_has_type(*variant_value_out, "int32", "integer"));
    EXPECT_TRUE(rpc_variant_one_of_case_has_type(*variant_value_out, "string", "string"));
    EXPECT_TRUE(required_contains(out_schema_json, "variant_value_out"));

    const auto* json_value_out = find_member(*out_properties, "json_value_out");
    ASSERT_NE(json_value_out, nullptr);
    EXPECT_FALSE(has_member(*json_value_out, "type"));
    EXPECT_TRUE(required_contains(out_schema_json, "json_value_out"));

    const auto* optional_json_value_out = find_member(*out_properties, "optional_json_value_out");
    ASSERT_NE(optional_json_value_out, nullptr);
    EXPECT_FALSE(has_member(*optional_json_value_out, "type"));
    EXPECT_FALSE(required_contains(out_schema_json, "optional_json_value_out"));
}

TEST(
    JSONSchemaValidationTest,
    StructGetSchemaBuildsFocusedDocument)
{
    json::v1::object schema_json;
    ASSERT_NO_THROW(schema_json = json::v1::parse(xxx::optional_variant_json_holder::get_schema()));

    const auto* schema_uri = find_member(schema_json, "$schema");
    ASSERT_NE(schema_uri, nullptr);
    EXPECT_EQ(schema_uri->get<std::string>(), "http://json-schema.org/draft-07/schema#");

    const auto* ref = find_member(schema_json, "$ref");
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->get<std::string>(), "#/definitions/xxx_optional_variant_json_holder");

    const auto* definitions = find_member(schema_json, "definitions");
    ASSERT_NE(definitions, nullptr);
    ASSERT_EQ(definitions->get_type(), json::v1::object::type::map_type);
    EXPECT_TRUE(has_member(*definitions, "xxx_optional_variant_json_holder"));
    EXPECT_FALSE(has_member(*definitions, "xxx_i_foo_exchange_optional_variant_json_send"));

    json::v1::object full_module_schema;
    ASSERT_NO_THROW(full_module_schema = json::v1::parse(canopy::generated_schema::example_shared_idl_json_schema()));
    const auto* full_definitions = find_member(full_module_schema, "definitions");
    ASSERT_NE(full_definitions, nullptr);
    EXPECT_TRUE(has_member(*full_definitions, "xxx_optional_variant_json_holder"));
    EXPECT_TRUE(has_member(*full_definitions, "xxx_i_foo_exchange_optional_variant_json_send"));

    EXPECT_THROW((void)xxx::optional_variant_json_holder::get_schema(rpc::encoding::yas_binary), std::invalid_argument);
}

TEST(
    JSONSchemaValidationTest,
    InterfaceGetSchemaBuildsScopedFunctionDefinitions)
{
    json::v1::object schema_json;
    ASSERT_NO_THROW(schema_json = json::v1::parse(xxx::i_foo::get_schema()));

    const auto* ref = find_member(schema_json, "$ref");
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->get<std::string>(), "#/definitions/xxx_i_foo");

    const auto* definitions = find_member(schema_json, "definitions");
    ASSERT_NE(definitions, nullptr);
    ASSERT_EQ(definitions->get_type(), json::v1::object::type::map_type);
    EXPECT_TRUE(has_member(*definitions, "xxx_i_foo"));
    EXPECT_TRUE(has_member(*definitions, "exchange_rpc_optional_holder_send"));
    EXPECT_TRUE(has_member(*definitions, "exchange_rpc_optional_holder_receive"));
    EXPECT_FALSE(has_member(*definitions, "xxx_i_foo_exchange_rpc_optional_holder_send"));
    EXPECT_FALSE(has_member(*definitions, "xxx_i_foo_exchange_rpc_optional_holder_receive"));
    EXPECT_TRUE(has_member(*definitions, "xxx_rpc_optional_holder"));

    const auto* interface_schema = find_member(*definitions, "xxx_i_foo");
    ASSERT_NE(interface_schema, nullptr);
    const auto* interface_properties = find_member(*interface_schema, "properties");
    ASSERT_NE(interface_properties, nullptr);
    const auto* method_schema = find_member(*interface_properties, "exchange_rpc_optional_holder");
    ASSERT_NE(method_schema, nullptr);
    EXPECT_TRUE(required_contains(*method_schema, "send"));
    EXPECT_TRUE(required_contains(*method_schema, "receive"));

    const auto* method_properties = find_member(*method_schema, "properties");
    ASSERT_NE(method_properties, nullptr);
    const auto* send_schema = find_member(*method_properties, "send");
    ASSERT_NE(send_schema, nullptr);
    const auto* send_ref = find_member(*send_schema, "$ref");
    ASSERT_NE(send_ref, nullptr);
    EXPECT_EQ(send_ref->get<std::string>(), "#/definitions/exchange_rpc_optional_holder_send");

    const auto* receive_schema = find_member(*method_properties, "receive");
    ASSERT_NE(receive_schema, nullptr);
    const auto* receive_ref = find_member(*receive_schema, "$ref");
    ASSERT_NE(receive_ref, nullptr);
    EXPECT_EQ(receive_ref->get<std::string>(), "#/definitions/exchange_rpc_optional_holder_receive");

    EXPECT_THROW((void)xxx::i_foo::get_schema(rpc::encoding::protocol_buffers), std::invalid_argument);
}

TEST(
    JSONSchemaValidationTest,
    RpcOptionalSchemaMappings)
{
    const auto fi = xxx::i_foo::get_function_info();
    auto it = std::find_if(
        fi.begin(), fi.end(), [](const auto& func) { return func.name == "exchange_rpc_optional_holder"; });
    ASSERT_NE(it, fi.end());

    json::v1::object schema_json;
    ASSERT_NO_THROW(schema_json = json::v1::parse(it->in_json_schema));

    const auto* properties = find_member(schema_json, "properties");
    ASSERT_NE(properties, nullptr);
    const auto* in_val = find_member(*properties, "in_val");
    ASSERT_NE(in_val, nullptr);
    EXPECT_TRUE(required_contains(*in_val, "required_int"));
    EXPECT_TRUE(required_contains(*in_val, "required_string"));
    EXPECT_FALSE(required_contains(*in_val, "optional_int"));
    EXPECT_FALSE(required_contains(*in_val, "optional_string"));
    EXPECT_FALSE(required_contains(*in_val, "optional_json_value"));

    expect_valid_schema_instance(
        schema_json, json::v1::parse(R"json({"in_val":{"required_int":7,"required_string":"required"}})json"));
    expect_valid_schema_instance(
        schema_json,
        json::v1::parse(
            R"json({"in_val":{"required_int":7,"required_string":"required","optional_int":null,"optional_string":null,"optional_json_value":null}})json"));

    const std::string missing_nested_optionals_payload
        = R"json({"in_val":{"required_int":7,"required_string":"required"}})json";
    xxx::rpc_optional_holder decoded_holder;
    auto err = xxx::i_foo::stub_deserialiser<rpc::serialiser::yas, rpc::encoding>::exchange_rpc_optional_holder(
        decoded_holder, rpc::byte_span(missing_nested_optionals_payload), rpc::encoding::yas_json);
    EXPECT_EQ(err, rpc::error::OK());
    EXPECT_EQ(decoded_holder.required_int, 7);
    EXPECT_EQ(decoded_holder.required_string, "required");
    EXPECT_FALSE(decoded_holder.optional_int.has_value());
    EXPECT_FALSE(decoded_holder.optional_string.has_value());
    EXPECT_FALSE(decoded_holder.optional_json_value.has_value());

    it = std::find_if(fi.begin(), fi.end(), [](const auto& func) { return func.name == "exchange_rpc_optional_values"; });
    ASSERT_NE(it, fi.end());
    ASSERT_NO_THROW(schema_json = json::v1::parse(it->in_json_schema));
    properties = find_member(schema_json, "properties");
    ASSERT_NE(properties, nullptr);

    const auto* optional_int = find_member(*properties, "optional_int");
    ASSERT_NE(optional_int, nullptr);
    EXPECT_TRUE(one_of_contains_schema_type(*optional_int, "integer"));
    EXPECT_TRUE(one_of_contains_schema_type(*optional_int, "null"));
    EXPECT_FALSE(required_contains(schema_json, "optional_int"));

    const auto* optional_json_value = find_member(*properties, "optional_json_value");
    ASSERT_NE(optional_json_value, nullptr);
    EXPECT_FALSE(has_member(*optional_json_value, "type"));
    EXPECT_FALSE(required_contains(schema_json, "optional_json_value"));

    expect_valid_schema_instance(schema_json, json::v1::parse(R"json({})json"));
    expect_valid_schema_instance(
        schema_json, json::v1::parse(R"json({"optional_int":null,"optional_json_value":null})json"));

    const std::string missing_top_level_optionals_payload = R"json({})json";
    rpc::optional<int32_t> decoded_optional_int;
    rpc::optional<json::v1::object> decoded_optional_json_value;
    err = xxx::i_foo::stub_deserialiser<rpc::serialiser::yas, rpc::encoding>::exchange_rpc_optional_values(
        decoded_optional_int,
        decoded_optional_json_value,
        rpc::byte_span(missing_top_level_optionals_payload),
        rpc::encoding::yas_json);
    EXPECT_EQ(err, rpc::error::OK());
    EXPECT_FALSE(decoded_optional_int.has_value());
    EXPECT_FALSE(decoded_optional_json_value.has_value());
}

TEST(
    JSONSchemaValidationTest,
    IDLGeneratedRecursiveRefsValidateNestedInstances)
{
    json::v1::object idl_schema;
    ASSERT_NO_THROW(idl_schema = json::v1::parse(schema_cycle::config_node::get_schema()));

    const auto* root_ref = find_member(idl_schema, "$ref");
    ASSERT_NE(root_ref, nullptr);
    ASSERT_EQ(root_ref->get_type(), json::v1::object::type::string_type);
    EXPECT_EQ(root_ref->get<std::string>(), "#/definitions/schema_cycle_config_node");

    const auto* definitions = find_member(idl_schema, "definitions");
    ASSERT_NE(definitions, nullptr);
    ASSERT_EQ(definitions->get_type(), json::v1::object::type::map_type);

    const auto* node_schema = find_member(*definitions, "schema_cycle_config_node");
    ASSERT_NE(node_schema, nullptr);

    const auto* properties = find_member(*node_schema, "properties");
    ASSERT_NE(properties, nullptr);
    const auto* children = find_member(*properties, "children");
    ASSERT_NE(children, nullptr);
    const auto* items = find_member(*children, "items");
    ASSERT_NE(items, nullptr);
    const auto* ref = find_member(*items, "$ref");
    ASSERT_NE(ref, nullptr);
    ASSERT_EQ(ref->get_type(), json::v1::object::type::string_type);
    EXPECT_EQ(ref->get<std::string>(), "#/definitions/schema_cycle_config_node");

    const json::v1::schema::schema_validator validator(idl_schema);

    EXPECT_TRUE(validator.validate(json::v1::parse(R"json({
        "name": "runtime",
        "children": [
            {
                "name": "tcp_coroutine",
                "children": [
                    {"name": "queue-pair", "children": []}
                ]
            }
        ]
    })json")));

    const auto invalid_result = validator.validate(json::v1::parse(R"json({
        "name": "runtime",
        "children": [
            {"name": 7}
        ]
    })json"));
    EXPECT_FALSE(invalid_result);
}

// Note: main() function now handled by GTest framework
// Tests are automatically discovered and run by gtest_main
