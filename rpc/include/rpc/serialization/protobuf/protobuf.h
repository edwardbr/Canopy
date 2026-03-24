/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <vector>
#include <string>
#include <cstdint>

// Forward declarations for protobuf types
namespace google
{
    namespace protobuf
    {
        template<typename T> class RepeatedField;
    }
}

namespace rpc
{
    namespace serialization
    {
        namespace protobuf
        {
            // Helper to serialize std::vector<uint8_t> to protobuf bytes field (std::string)
            inline void serialize_bytes(
                const std::vector<uint8_t>& data,
                std::string& proto_bytes)
            {
                proto_bytes.assign(reinterpret_cast<const char*>(data.data()), data.size());
            }

            // Helper to deserialize protobuf bytes field (std::string) to std::vector<uint8_t>
            inline void deserialize_bytes(
                const std::string& proto_bytes,
                std::vector<uint8_t>& data)
            {
                data.assign(proto_bytes.begin(), proto_bytes.end());
            }

            // Overloads for std::vector<char> (char and uint8_t are compatible)
            inline void serialize_bytes(
                const std::vector<int8_t>& data,
                std::string& proto_bytes)
            {
                proto_bytes.assign(reinterpret_cast<const char*>(data.data()), data.size());
            }

            inline void deserialize_bytes(
                const std::string& proto_bytes,
                std::vector<int8_t>& data)
            {
                data.assign(proto_bytes.begin(), proto_bytes.end());
            }

            // Overloads for std::vector<char> (char and uint8_t are compatible)
            inline void serialize_bytes(
                const std::vector<char>& data,
                std::string& proto_bytes)
            {
                proto_bytes.assign(data.data(), data.size());
            }

            inline void deserialize_bytes(
                const std::string& proto_bytes,
                std::vector<char>& data)
            {
                data.assign(proto_bytes.begin(), proto_bytes.end());
            }

            // Helper to serialize std::vector<T> to protobuf repeated field for integer types
            template<typename T>
            inline void serialize_integer_vector(
                const std::vector<T>& data,
                google::protobuf::RepeatedField<T>& proto_field)
            {
                static_assert(std::is_integral<T>::value, "serialize_integer_vector requires an integral type");
                proto_field.Clear();
                proto_field.Reserve(data.size());
                for (const auto& val : data)
                {
                    proto_field.Add(val);
                }
            }

            // Helper to deserialize protobuf repeated field to std::vector<T> for integer types
            template<typename T>
            inline void deserialize_integer_vector(
                const google::protobuf::RepeatedField<T>& proto_field,
                std::vector<T>& data)
            {
                static_assert(std::is_integral<T>::value, "deserialize_integer_vector requires an integral type");
                data.clear();
                data.reserve(proto_field.size());
                for (int i = 0; i < proto_field.size(); ++i)
                {
                    data.push_back(proto_field.Get(i));
                }
            }

        } // namespace protobuf
    } // namespace serialization
} // namespace rpc
