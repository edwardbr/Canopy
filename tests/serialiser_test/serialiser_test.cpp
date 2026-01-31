/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>

#include <rpc/rpc.h>
#include <example_shared/example_shared.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        printf("[DEBUG] %s\n", message.c_str());
        break;
    case 1:
        printf("[TRACE] %s\n", message.c_str());
        break;
    case 2:
        printf("[INFO] %s\n", message.c_str());
        break;
    case 3:
        printf("[WARN] %s\n", message.c_str());
        break;
    case 4:
        printf("[ERROR] %s\n", message.c_str());
        break;
    case 5:
        printf("[CRITICAL] %s\n", message.c_str());
        break;
    default:
        printf("[LOG %d] %s\n", level, message.c_str());
        break;
    }
}

// Test fixture for serialiser tests
class SerialiserTest : public ::testing::Test
{
protected:
    void SetUp() override { }

    void TearDown() override { }
};

// Test to_yas_json serialization with generated structure
TEST_F(SerialiserTest, ToYasJson)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    auto result = rpc::to_yas_json(obj);

    EXPECT_FALSE(result.empty());
    EXPECT_NE(result[0], '\0'); // JSON should have content
}

// Test to_yas_binary serialization with generated structure
TEST_F(SerialiserTest, ToYasBinary)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    auto result = rpc::to_yas_binary(obj);

    EXPECT_FALSE(result.empty());
}

// Test to_compressed_yas_binary serialization with generated structure
TEST_F(SerialiserTest, ToCompressedYasBinary)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    auto result = rpc::to_compressed_yas_binary(obj);

    EXPECT_FALSE(result.empty());
}

// Test to_protobuf serialization with generated structure
TEST_F(SerialiserTest, ToProtobuf)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test";

    auto result = rpc::to_protobuf(obj);

    EXPECT_FALSE(result.empty());
    // Verify size is non-zero (actual size depends on protobuf implementation)
    EXPECT_GT(result.size(), 0u);
}

// Test from_yas_json deserialization with generated structure
TEST_F(SerialiserTest, FromYasJson)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    auto serialized = rpc::to_yas_json(obj);
    rpc::span data_span(serialized);

    xxx::something_complicated deserialized;
    auto error = rpc::from_yas_json(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, 42);
    EXPECT_EQ(deserialized.string_val, "test_string");
}

// Test from_yas_binary deserialization with generated structure
TEST_F(SerialiserTest, FromYasBinary)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    auto serialized = rpc::to_yas_binary(obj);
    rpc::span data_span(serialized);

    xxx::something_complicated deserialized;
    auto error = rpc::from_yas_binary(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, 42);
    EXPECT_EQ(deserialized.string_val, "test_string");
}

// Test from_yas_compressed_binary deserialization with generated structure
TEST_F(SerialiserTest, FromYasCompressedBinary)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    auto serialized = rpc::to_compressed_yas_binary(obj);
    rpc::span data_span(serialized);

    xxx::something_complicated deserialized;
    auto error = rpc::from_yas_compressed_binary(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, 42);
    EXPECT_EQ(deserialized.string_val, "test_string");
}

// Test from_protobuf deserialization with generated structure
TEST_F(SerialiserTest, FromProtobuf)
{
    xxx::something_complicated obj;
    obj.int_val = 100;
    obj.string_val = "hello";

    auto serialized = rpc::to_protobuf(obj);
    rpc::span data_span(serialized);

    xxx::something_complicated deserialized;
    auto error = rpc::from_protobuf(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, 100);
    EXPECT_EQ(deserialized.string_val, "hello");
}

// Test serialise function with all encodings using generated structure
TEST_F(SerialiserTest, SerialiseAllEncodings)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "serialize_test";

    // Test yas_json
    auto json_result = rpc::serialise(obj, rpc::encoding::yas_json);
    EXPECT_FALSE(json_result.empty());

    // Test yas_binary
    auto binary_result = rpc::serialise(obj, rpc::encoding::yas_binary);
    EXPECT_FALSE(binary_result.empty());

    // Test yas_compressed_binary
    auto compressed_result = rpc::serialise(obj, rpc::encoding::yas_compressed_binary);
    EXPECT_FALSE(compressed_result.empty());

    // Test protocol_buffers
    auto protobuf_result = rpc::serialise(obj, rpc::encoding::protocol_buffers);
    EXPECT_FALSE(protobuf_result.empty());
}

// Test deserialise function with all encodings using generated structure
TEST_F(SerialiserTest, DeserialiseAllEncodings)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "deserialize_test";

    // Test yas_json
    {
        auto serialized = rpc::serialise(obj, rpc::encoding::yas_json);
        rpc::span data_span(serialized);
        xxx::something_complicated deserialized;
        auto error = rpc::deserialise(rpc::encoding::yas_json, data_span, deserialized);
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(deserialized.int_val, obj.int_val);
        EXPECT_EQ(deserialized.string_val, obj.string_val);
    }

    // Test yas_binary
    {
        auto serialized = rpc::serialise(obj, rpc::encoding::yas_binary);
        rpc::span data_span(serialized);
        xxx::something_complicated deserialized;
        auto error = rpc::deserialise(rpc::encoding::yas_binary, data_span, deserialized);
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(deserialized.int_val, obj.int_val);
        EXPECT_EQ(deserialized.string_val, obj.string_val);
    }

    // Test yas_compressed_binary
    {
        auto serialized = rpc::serialise(obj, rpc::encoding::yas_compressed_binary);
        rpc::span data_span(serialized);
        xxx::something_complicated deserialized;
        auto error = rpc::deserialise(rpc::encoding::yas_compressed_binary, data_span, deserialized);
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(deserialized.int_val, obj.int_val);
        EXPECT_EQ(deserialized.string_val, obj.string_val);
    }

    // Test protocol_buffers
    {
        auto serialized = rpc::serialise(obj, rpc::encoding::protocol_buffers);
        rpc::span data_span(serialized);
        xxx::something_complicated deserialized;
        auto error = rpc::deserialise(rpc::encoding::protocol_buffers, data_span, deserialized);
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(deserialized.int_val, obj.int_val);
        EXPECT_EQ(deserialized.string_val, obj.string_val);
    }
}

// Test get_saved_size function with all encodings
TEST_F(SerialiserTest, GetSavedSizeAllEncodings)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "size_test";

    // Test yas_json size
    auto json_size = rpc::get_saved_size(obj, rpc::encoding::yas_json);
    EXPECT_GT(json_size, 0u);

    // Test yas_binary size
    auto binary_size = rpc::get_saved_size(obj, rpc::encoding::yas_binary);
    EXPECT_GT(binary_size, 0u);

    // Test yas_compressed_binary size
    auto compressed_size = rpc::get_saved_size(obj, rpc::encoding::yas_compressed_binary);
    EXPECT_GT(compressed_size, 0u);

    // Test protocol_buffers size
    auto protobuf_size = rpc::get_saved_size(obj, rpc::encoding::protocol_buffers);
    EXPECT_GT(protobuf_size, 0u);

    // Verify sizes are consistent with actual serialized data
    auto json_result = rpc::serialise(obj, rpc::encoding::yas_json);
    EXPECT_EQ(json_size, json_result.size());

    auto binary_result = rpc::serialise(obj, rpc::encoding::yas_binary);
    EXPECT_EQ(binary_size, binary_result.size());

    auto protobuf_result = rpc::serialise(obj, rpc::encoding::protocol_buffers);
    EXPECT_EQ(protobuf_size, protobuf_result.size());
}

// Test with complex structure (something_complicated from example_shared)
TEST_F(SerialiserTest, ComplexStructProtobuf)
{
    xxx::something_complicated obj;
    obj.int_val = 123;
    obj.string_val = "complex_test";

    // Test protobuf serialization using serialise function
    auto serialized = rpc::serialise(obj, rpc::encoding::protocol_buffers);
    EXPECT_FALSE(serialized.empty());

    // Test deserialization
    rpc::span data_span(serialized);
    xxx::something_complicated deserialized;
    auto error = rpc::from_protobuf(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, 123);
    EXPECT_EQ(deserialized.string_val, "complex_test");
}

// Test with more complex structure (something_more_complicated from example_shared)
TEST_F(SerialiserTest, NestedStructProtobuf)
{
    xxx::something_more_complicated obj;
    obj.vector_val.push_back({1, "first"});
    obj.vector_val.push_back({2, "second"});
    obj.map_val["key1"] = {10, "map_first"};
    obj.map_val["key2"] = {20, "map_second"};

    // Test protobuf serialization using serialise function
    auto serialized = rpc::serialise(obj, rpc::encoding::protocol_buffers);
    EXPECT_FALSE(serialized.empty());

    // Test deserialization
    rpc::span data_span(serialized);
    xxx::something_more_complicated deserialized;
    auto error = rpc::from_protobuf(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.vector_val.size(), 2u);
    EXPECT_EQ(deserialized.map_val.size(), 2u);
}

// Test roundtrip for all serialiser functions with generated structure
TEST_F(SerialiserTest, RoundtripAllEncodings)
{
    xxx::something_complicated original;
    original.int_val = 999;
    original.string_val = "roundtrip_test";

    const std::vector<rpc::encoding> encodings = {rpc::encoding::yas_json,
        rpc::encoding::yas_binary,
        rpc::encoding::yas_compressed_binary,
        rpc::encoding::protocol_buffers};

    for (auto enc : encodings)
    {
        // Serialize
        auto serialized = rpc::serialise(original, enc);
        EXPECT_FALSE(serialized.empty()) << "Serialization failed for encoding: " << static_cast<int>(enc);

        // Deserialize
        rpc::span data_span(serialized);
        xxx::something_complicated deserialized;
        auto error = rpc::deserialise(enc, data_span, deserialized);
        EXPECT_TRUE(error.empty()) << "Deserialization failed for encoding: " << static_cast<int>(enc)
                                   << " error: " << error;
        EXPECT_EQ(deserialized.int_val, original.int_val) << "int_val mismatch for encoding: " << static_cast<int>(enc);
        EXPECT_EQ(deserialized.string_val, original.string_val)
            << "string_val mismatch for encoding: " << static_cast<int>(enc);
    }
}

// Test with template structure (test_template<int>)
TEST_F(SerialiserTest, TemplateStructProtobuf)
{
    xxx::test_template<int> obj;
    obj.type_t = 42;

    // Test protobuf serialization
    auto serialized = rpc::serialise(obj, rpc::encoding::protocol_buffers);
    EXPECT_FALSE(serialized.empty());

    // Test deserialization
    rpc::span data_span(serialized);
    xxx::test_template<int> deserialized;
    auto error = rpc::from_protobuf(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.type_t, 42);
}

// Test protobuf_saved_size function
TEST_F(SerialiserTest, ProtobufSavedSize)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test";

    auto size = rpc::protobuf_saved_size(obj);
    EXPECT_GT(size, 0u);

    // Verify it matches the actual serialized size
    auto serialized = rpc::to_protobuf(obj);
    EXPECT_EQ(size, serialized.size());
}

// Test edge cases - empty string
TEST_F(SerialiserTest, EmptyStringProtobuf)
{
    xxx::something_complicated obj;
    obj.int_val = 0;
    obj.string_val = "";

    auto serialized = rpc::to_protobuf(obj);
    rpc::span data_span(serialized);

    xxx::something_complicated deserialized;
    auto error = rpc::from_protobuf(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, 0);
    EXPECT_EQ(deserialized.string_val, "");
}

// Test edge cases - large values
TEST_F(SerialiserTest, LargeValuesProtobuf)
{
    xxx::something_complicated obj;
    obj.int_val = INT_MAX;
    obj.string_val = std::string(1000, 'x');

    auto serialized = rpc::to_protobuf(obj);
    rpc::span data_span(serialized);

    xxx::something_complicated deserialized;
    auto error = rpc::from_protobuf(data_span, deserialized);

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(deserialized.int_val, INT_MAX);
    EXPECT_EQ(deserialized.string_val, std::string(1000, 'x'));
}

// Test to_yas_json with std::array
TEST_F(SerialiserTest, ToYasJsonWithArray)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    // Test with sufficiently large array
    std::array<char, 200> json_result = rpc::to_yas_json<std::array<char, 200>>(obj);
    EXPECT_FALSE(json_result.empty());

    // Test with array that's too small - should throw exception
    EXPECT_THROW((rpc::to_yas_json<std::array<char, 10>>(obj)), std::runtime_error);
}

// Test to_yas_binary with std::array
TEST_F(SerialiserTest, ToYasBinaryWithArray)
{
    xxx::something_complicated obj;
    obj.int_val = 42;
    obj.string_val = "test_string";

    // Test with sufficiently large array
    std::array<char, 200> binary_result = rpc::to_yas_binary<std::array<char, 200>>(obj);
    EXPECT_FALSE(binary_result.empty());

    // Test with array that's too small - should throw exception
    EXPECT_THROW((rpc::to_yas_binary<std::array<char, 10>>(obj)), std::runtime_error);
}
