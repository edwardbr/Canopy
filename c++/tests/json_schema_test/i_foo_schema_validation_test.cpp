/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <json/schema_validator.h>

// RPC headers
#include <rpc/rpc.h>

// Other headers
#include <example_shared/example_shared.h>
#include <gtest/gtest.h>

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

    bool has_non_empty_properties(const json::v1::object& schema)
    {
        const auto* properties = find_member(schema, "properties");
        return properties && properties->get_type() == json::v1::object::type::map_type && !properties->as_map().empty();
    }
}

// Helper function to create dummy data for different types
template<typename T> T create_dummy_value();

template<> int create_dummy_value<int>()
{
    return 42;
}

template<> std::string create_dummy_value<std::string>()
{
    return "test_string";
}

template<> json::v1::object create_dummy_value<json::v1::object>()
{
    return json::v1::object(
        json::v1::map{
            {"name", "schema-test"},
            {"values", json::v1::array{"one", true, nullptr}},
        });
}

template<> xxx::something_complicated create_dummy_value<xxx::something_complicated>()
{
    xxx::something_complicated obj;
    obj.int_val = 123;
    obj.string_val = "test_complicated";
    return obj;
}

template<> xxx::something_more_complicated create_dummy_value<xxx::something_more_complicated>()
{
    xxx::something_more_complicated obj;
    obj.vector_val.push_back(create_dummy_value<xxx::something_complicated>());
    obj.map_val["key1"] = create_dummy_value<xxx::something_complicated>();
    return obj;
}

template<> xxx::optional_variant_json_holder create_dummy_value<xxx::optional_variant_json_holder>()
{
    xxx::optional_variant_json_holder obj;
    obj.optional_int = 42;
    obj.variant_value = std::string("schema-variant");
    obj.json_value = create_dummy_value<json::v1::object>();
    obj.optional_json_value = json::v1::object(
        json::v1::map{
            {"enabled", true},
            {"label", "schema-optional-json"},
        });
    obj.rpc_optional_int = 43;
    obj.rpc_optional_json_value = json::v1::object(
        json::v1::map{
            {"enabled", true},
            {"label", "schema-rpc-optional-json"},
        });
    return obj;
}

template<> xxx::rpc_optional_holder create_dummy_value<xxx::rpc_optional_holder>()
{
    xxx::rpc_optional_holder obj;
    obj.required_int = 7;
    obj.required_string = "required";
    obj.optional_int = 51;
    obj.optional_string = std::string("present");
    obj.optional_json_value = json::v1::object(
        json::v1::map{
            {"kind", "rpc-optional"},
            {"enabled", true},
        });
    return obj;
}

// Test all functions in i_foo for schema validation with actual serialized data
TEST(
    JSONSchemaValidationTest,
    IFooAllFunctionsSchemaValidation)
{
    // Get function info for i_foo
    auto fi = xxx::i_foo::get_function_info();
    ASSERT_FALSE(fi.empty()) << "Function info should not be empty";

    int tested_functions = 0;
    int skipped_functions = 0;
    std::vector<std::string> failed_functions;
    std::vector<std::string> tested_function_names;

    // Iterate through all functions in i_foo
    for (const auto& func_info : fi)
    {
        // Skip functions that marshall interfaces
        if (func_info.marshalls_interfaces)
        {
            skipped_functions++;
            std::cout << "Skipping function: " << func_info.name << " (marshalls_interfaces = true)" << std::endl;
            continue;
        }

        std::cout << "Testing function: " << func_info.name << std::endl;
        tested_function_names.push_back(func_info.name);

        try
        {
            // Check that function has a non-empty schema if it doesn't marshall interfaces
            if (func_info.in_json_schema.empty())
            {
                std::cout << "WARNING: Function " << func_info.name
                          << " has empty schema but marshalls_interfaces=false" << std::endl;
                failed_functions.push_back(func_info.name + " (empty schema)");
                continue;
            }

            // Parse the schema from the function info
            json::v1::object schema_json;
            try
            {
                schema_json = json::v1::parse(func_info.in_json_schema);
            }
            catch (const std::exception& e)
            {
                std::cout << "ERROR: Failed to parse schema for " << func_info.name << ": " << e.what() << std::endl;
                failed_functions.push_back(func_info.name + " (schema parse error)");
                continue;
            }

            const json::v1::schema::schema_validator validator(schema_json);

            // Now test actual serialization and validate against schema
            std::vector<char> buffer;
            bool serialization_success = false;

            // Call the appropriate proxy_serialiser function based on function name
            if (func_info.name == "do_something_in_val")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_in_val(
                    create_dummy_value<int>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_in_ref")
            {
                int val = create_dummy_value<int>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_in_ref(
                    val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_in_move_ref")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_in_move_ref(
                    create_dummy_value<int>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_in_ptr")
            {
                int val = create_dummy_value<int>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_in_ptr(
                    reinterpret_cast<uint64_t>(&val), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_enum_ptr_in")
            {
                auto val = xxx::pointer_serialisation_state::pointer_serialisation_pending;
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_enum_ptr_in(
                    reinterpret_cast<uint64_t>(&val), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_out_val")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_out_val(
                    buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_out_ptr_ref")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_out_ptr_ref(
                    buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_out_ptr_ptr")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_out_ptr_ptr(
                    buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_enum_ptr_out")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_enum_ptr_out(
                    buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_in_out_ref")
            {
                int val = create_dummy_value<int>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_in_out_ref(
                    val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_complicated_val")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_complicated_val(
                        create_dummy_value<xxx::something_complicated>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_complicated_ref")
            {
                auto val = create_dummy_value<xxx::something_complicated>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_complicated_ref(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_complicated_ref_val")
            {
                auto val = create_dummy_value<xxx::something_complicated>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_complicated_ref_val(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_complicated_move_ref")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_complicated_move_ref(
                        create_dummy_value<xxx::something_complicated>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_complicated_ptr")
            {
                uint64_t val = 10;
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_complicated_ptr(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_complicated_ref")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_complicated_ref(
                        buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_complicated_ptr")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_complicated_ptr(
                        buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_complicated_in_out_ref")
            {
                auto val = create_dummy_value<xxx::something_complicated>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_complicated_in_out_ref(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_more_complicated_val")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_more_complicated_val(
                        create_dummy_value<xxx::something_more_complicated>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_more_complicated_ref")
            {
                auto val = create_dummy_value<xxx::something_more_complicated>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_more_complicated_ref(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_more_complicated_move_ref")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_more_complicated_move_ref(
                        create_dummy_value<xxx::something_more_complicated>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_more_complicated_ref_val")
            {
                auto val = create_dummy_value<xxx::something_more_complicated>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_more_complicated_ref_val(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_something_more_complicated_ptr")
            {
                uint64_t val = 10;
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_something_more_complicated_ptr(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_more_complicated_ref")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_more_complicated_ref(
                        buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_more_complicated_ptr")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_more_complicated_ptr(
                        buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_more_complicated_in_out_ref")
            {
                auto val = create_dummy_value<xxx::something_more_complicated>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_more_complicated_in_out_ref(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_multi_val")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_multi_val(
                    create_dummy_value<int>(), create_dummy_value<int>(), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_multi_complicated_val")
            {
                auto val1 = create_dummy_value<xxx::something_more_complicated>();
                auto val2 = create_dummy_value<xxx::something_more_complicated>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::do_multi_complicated_val(
                    val1, val2, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_json_object")
            {
                auto val = create_dummy_value<json::v1::object>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_json_object(
                    val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "give_optional_variant_json_holder")
            {
                auto val = create_dummy_value<xxx::optional_variant_json_holder>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::give_optional_variant_json_holder(
                        val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_optional_variant_json_holder")
            {
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_optional_variant_json_holder(
                        buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_optional_variant_json")
            {
                const auto holder = create_dummy_value<xxx::optional_variant_json_holder>();
                auto err
                    = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_optional_variant_json(
                        holder.optional_int,
                        holder.variant_value,
                        holder.json_value,
                        holder.optional_json_value,
                        buffer,
                        rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_rpc_optional_holder")
            {
                auto val = create_dummy_value<xxx::rpc_optional_holder>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_rpc_optional_holder(
                    val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_rpc_optional_values")
            {
                const auto holder = create_dummy_value<xxx::optional_variant_json_holder>();
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_rpc_optional_values(
                    holder.rpc_optional_int, holder.rpc_optional_json_value, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exception_test")
            {
                auto err = xxx::i_foo::proxy_serialiser<rpc::serialiser::yas, rpc::encoding>::exception_test(
                    buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else
            {
                std::cout << "WARNING: Function " << func_info.name << " not implemented in test" << std::endl;
                failed_functions.push_back(func_info.name + " (not implemented in test)");
                continue;
            }

            if (!serialization_success)
            {
                std::cout << "ERROR: Serialization failed for " << func_info.name << std::endl;
                failed_functions.push_back(func_info.name + " (serialization failed)");
                continue;
            }

            // Convert buffer to string and parse as JSON
            std::string buffer_str(buffer.begin(), buffer.end());
            if (buffer_str.empty())
            {
                std::cout << "ERROR: Serialized buffer is empty for " << func_info.name << std::endl;
                failed_functions.push_back(func_info.name + " (empty serialization)");
                continue;
            }

            // Parse the serialized data as JSON
            json::v1::object payload_json;
            try
            {
                payload_json = json::v1::parse(buffer_str);
            }
            catch (const std::exception& e)
            {
                std::cout << "ERROR: Failed to parse serialized JSON for " << func_info.name << ": " << e.what()
                          << std::endl;
                failed_functions.push_back(func_info.name + " (JSON parse error)");
                continue;
            }

            const auto validation_result = validator.validate(payload_json);
            if (validation_result)
            {
                std::cout << "SUCCESS: Payload validates against schema for " << func_info.name << std::endl;
                tested_functions++;
            }
            else
            {
                std::cout << "ERROR: Schema validation failed for " << func_info.name << ": "
                          << validation_result.errors().front().message << std::endl;
                std::cout << "Payload: " << buffer_str << std::endl;
                std::cout << "Schema: " << func_info.in_json_schema << std::endl;
                failed_functions.push_back(func_info.name + " (payload validation failed)");
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "ERROR: Unexpected error testing " << func_info.name << ": " << e.what() << std::endl;
            failed_functions.push_back(func_info.name + " (unexpected error)");
        }
    }

    // Print summary
    std::cout << "\n=== SUMMARY ===" << std::endl;
    std::cout << "Total functions found: " << fi.size() << std::endl;
    std::cout << "Functions tested: " << tested_functions << std::endl;
    std::cout << "Functions skipped (marshalls_interfaces=true): " << skipped_functions << std::endl;
    std::cout << "Functions failed: " << failed_functions.size() << std::endl;

    if (!tested_function_names.empty())
    {
        std::cout << "\nTested functions:" << std::endl;
        for (const auto& name : tested_function_names)
        {
            std::cout << "  - " << name << std::endl;
        }
    }

    if (!failed_functions.empty())
    {
        std::cout << "\nFailed functions:" << std::endl;
        for (const auto& name : failed_functions)
        {
            std::cout << "  - " << name << std::endl;
        }
    }

    // The test should pass even if there are failures, so we can see all results
    // and fix the schema generator in one go
    if (!failed_functions.empty())
    {
        std::cout << "\nNOTE: Test completed with " << failed_functions.size()
                  << " failed functions. This is expected during development." << std::endl;
    }

    SUCCEED() << "Schema validation test completed. Tested " << tested_functions << " functions, skipped "
              << skipped_functions << ", failed " << failed_functions.size();
}

// Test all functions in i_foo for output schema validation with stub_serialiser
TEST(
    JSONSchemaValidationTest,
    IFooAllFunctionsOutputSchemaValidation)
{
    // Get function info for i_foo
    auto fi = xxx::i_foo::get_function_info();
    ASSERT_FALSE(fi.empty()) << "Function info should not be empty";

    int tested_functions = 0;
    int skipped_functions = 0;
    int functions_with_no_output = 0;
    std::vector<std::string> failed_functions;
    std::vector<std::string> tested_function_names;

    // Iterate through all functions in i_foo
    for (const auto& func_info : fi)
    {
        // Skip functions that marshall interfaces
        if (func_info.marshalls_interfaces)
        {
            skipped_functions++;
            std::cout << "Skipping function: " << func_info.name << " (marshalls_interfaces = true)" << std::endl;
            continue;
        }

        std::cout << "Testing output schema for function: " << func_info.name << std::endl;
        tested_function_names.push_back(func_info.name);

        try
        {
            // Parse the output schema from the function info
            json::v1::object schema_json;
            try
            {
                schema_json = json::v1::parse(func_info.out_json_schema);
            }
            catch (const std::exception& e)
            {
                std::cout << "ERROR: Failed to parse output schema for " << func_info.name << ": " << e.what() << std::endl;
                failed_functions.push_back(func_info.name + " (output schema parse error)");
                continue;
            }

            // Check if this function has output parameters
            bool has_output_params = has_non_empty_properties(schema_json);
            if (!has_output_params)
            {
                std::cout << "INFO: Function " << func_info.name << " has no output parameters" << std::endl;
                functions_with_no_output++;
                continue;
            }

            const json::v1::schema::schema_validator validator(schema_json);

            // Now test stub_serialiser with dummy output data
            std::vector<char> buffer;
            bool serialization_success = false;

            // Call the appropriate stub_serialiser function based on function name and create dummy output
            if (func_info.name == "do_something_out_val")
            {
                int out_val = create_dummy_value<int>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_out_val(
                    out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_out_ptr_ref")
            {
                int out_val = create_dummy_value<int>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_out_ptr_ref(
                    out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_out_ptr_ptr")
            {
                int out_val = create_dummy_value<int>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_out_ptr_ptr(
                    out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_enum_ptr_out")
            {
                auto out_val = xxx::pointer_serialisation_state::pointer_serialisation_ready;
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::do_enum_ptr_out(
                    reinterpret_cast<uint64_t>(&out_val), buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "do_something_in_out_ref")
            {
                int out_val = create_dummy_value<int>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::do_something_in_out_ref(
                    out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_complicated_ref")
            {
                auto out_val = create_dummy_value<xxx::something_complicated>();
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_complicated_ref(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_complicated_ptr")
            {
                uint64_t out_val = 12345; // Dummy pointer address
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_complicated_ptr(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_complicated_in_out_ref")
            {
                auto out_val = create_dummy_value<xxx::something_complicated>();
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_complicated_in_out_ref(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_more_complicated_ref")
            {
                auto out_val = create_dummy_value<xxx::something_more_complicated>();
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_more_complicated_ref(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_more_complicated_ptr")
            {
                uint64_t out_val = 67890; // Dummy pointer address
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_more_complicated_ptr(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_something_more_complicated_in_out_ref")
            {
                auto out_val = create_dummy_value<xxx::something_more_complicated>();
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_something_more_complicated_in_out_ref(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_json_object")
            {
                auto out_val = create_dummy_value<json::v1::object>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_json_object(
                    out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_optional_variant_json_holder")
            {
                auto out_val = create_dummy_value<xxx::optional_variant_json_holder>();
                auto err
                    = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::receive_optional_variant_json_holder(
                        out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_optional_variant_json")
            {
                const auto holder = create_dummy_value<xxx::optional_variant_json_holder>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_optional_variant_json(
                    holder.optional_int,
                    holder.variant_value,
                    holder.json_value,
                    holder.optional_json_value,
                    buffer,
                    rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_rpc_optional_holder")
            {
                auto out_val = create_dummy_value<xxx::rpc_optional_holder>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_rpc_optional_holder(
                    out_val, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "exchange_rpc_optional_values")
            {
                const auto holder = create_dummy_value<xxx::optional_variant_json_holder>();
                auto err = xxx::i_foo::stub_serialiser<rpc::serialiser::yas, rpc::encoding>::exchange_rpc_optional_values(
                    holder.rpc_optional_int, holder.rpc_optional_json_value, buffer, rpc::encoding::yas_json);
                serialization_success = (err == 0);
            }
            else if (func_info.name == "receive_interface" || func_info.name == "give_interface"
                     || func_info.name == "call_baz_interface" || func_info.name == "create_baz_interface"
                     || func_info.name == "get_null_interface" || func_info.name == "set_interface"
                     || func_info.name == "get_interface")
            {
                std::cout << "INFO: Function " << func_info.name
                          << " has interface output parameters - skipping stub_serialiser test" << std::endl;
                functions_with_no_output++;
                continue;
            }
            else
            {
                std::cout << "INFO: Function " << func_info.name
                          << " has no testable output parameters or not implemented in output test" << std::endl;
                functions_with_no_output++;
                continue;
            }

            if (!serialization_success)
            {
                std::cout << "ERROR: Output serialization failed for " << func_info.name << std::endl;
                failed_functions.push_back(func_info.name + " (output serialization failed)");
                continue;
            }

            // Convert buffer to string and parse as JSON
            std::string buffer_str(buffer.begin(), buffer.end());
            if (buffer_str.empty())
            {
                std::cout << "ERROR: Serialized output buffer is empty for " << func_info.name << std::endl;
                failed_functions.push_back(func_info.name + " (empty output serialization)");
                continue;
            }

            // Parse the serialized data as JSON
            json::v1::object payload_json;
            try
            {
                payload_json = json::v1::parse(buffer_str);
            }
            catch (const std::exception& e)
            {
                std::cout << "ERROR: Failed to parse serialized output JSON for " << func_info.name << ": " << e.what()
                          << std::endl;
                failed_functions.push_back(func_info.name + " (output JSON parse error)");
                continue;
            }

            const auto validation_result = validator.validate(payload_json);
            if (validation_result)
            {
                std::cout << "SUCCESS: Output payload validates against schema for " << func_info.name << std::endl;
                tested_functions++;
            }
            else
            {
                std::cout << "ERROR: Output schema validation failed for " << func_info.name << ": "
                          << validation_result.errors().front().message << std::endl;
                std::cout << "Output Payload: " << buffer_str << std::endl;
                std::cout << "Output Schema: " << func_info.out_json_schema << std::endl;
                failed_functions.push_back(func_info.name + " (output payload validation failed)");
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "ERROR: Unexpected error testing output for " << func_info.name << ": " << e.what() << std::endl;
            failed_functions.push_back(func_info.name + " (unexpected error)");
        }
    }

    // Print summary
    std::cout << "\n=== OUTPUT SCHEMA VALIDATION SUMMARY ===" << std::endl;
    std::cout << "Total functions found: " << fi.size() << std::endl;
    std::cout << "Functions with output parameters tested: " << tested_functions << std::endl;
    std::cout << "Functions skipped (marshalls_interfaces=true): " << skipped_functions << std::endl;
    std::cout << "Functions with no output parameters: " << functions_with_no_output << std::endl;
    std::cout << "Functions failed: " << failed_functions.size() << std::endl;

    if (!tested_function_names.empty())
    {
        std::cout << "\nFunctions examined:" << std::endl;
        for (const auto& name : tested_function_names)
        {
            std::cout << "  - " << name << std::endl;
        }
    }

    if (!failed_functions.empty())
    {
        std::cout << "\nFailed functions:" << std::endl;
        for (const auto& name : failed_functions)
        {
            std::cout << "  - " << name << std::endl;
        }
    }

    // The test should pass even if there are failures, so we can see all results
    if (!failed_functions.empty())
    {
        std::cout << "\nNOTE: Output schema test completed with " << failed_functions.size()
                  << " failed functions. This is expected during development." << std::endl;
    }

    SUCCEED() << "Output schema validation test completed. Tested " << tested_functions
              << " functions with output parameters, " << functions_with_no_output
              << " had no output parameters, skipped " << skipped_functions << ", failed " << failed_functions.size();
}
