/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <assert.h>
#include <array>
#include <type_traits>

namespace rpc
{

    // Type trait to detect std::array and extract its parameters
    template<typename T> struct is_std_array : std::false_type
    {
    };

    template<typename T, size_t N> struct is_std_array<std::array<T, N>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_std_array_v = is_std_array<T>::value;

    // Helper to extract array element type and size
    template<typename T> struct array_traits;

    template<typename T, size_t N> struct array_traits<std::array<T, N>>
    {
        using value_type = T;
        static constexpr size_t size = N;
    };

    // note a serialiser may support more than one encoding
    namespace serialiser
    {
        class yas
        {
        };
        class protocol_buffers
        {
        };
        class flat_buffers
        {
        };
        class open_mpi
        {
        };
        // etc..
    };

    // Size calculation functions (declared first for use in serialization)
    template<typename T> uint64_t yas_json_saved_size(const T& obj)
    {
        YAS_WARNINGS_PUSH
        return yas::saved_size<::yas::mem | ::yas::json | ::yas::no_header>(obj);
        YAS_WARNINGS_POP
    }

    template<typename T> uint64_t yas_binary_saved_size(const T& obj)
    {
        YAS_WARNINGS_PUSH
        return yas::saved_size<::yas::mem | ::yas::binary | ::yas::no_header>(obj);
        YAS_WARNINGS_POP
    }

    template<typename T> uint64_t compressed_yas_binary_saved_size(const T& obj)
    {
        YAS_WARNINGS_PUSH
        return yas::saved_size<::yas::mem | ::yas::binary | ::yas::compacted | ::yas::no_header>(obj);
        YAS_WARNINGS_POP
    }

#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
    // note that this function is here for completeness but is not efficient as it requires serialisation to get size
    template<typename T> uint64_t protobuf_saved_size(const T& obj)
    {
        std::vector<char> buffer;
        obj.protobuf_serialise(buffer);
        return buffer.size();
    }
#endif

    // Serialization functions - work with both std::vector-like containers and std::array
    template<
        class OutputBlob = std::vector<std::uint8_t>,
        typename T>
    OutputBlob to_yas_json(const T& obj)
    {
        YAS_WARNINGS_PUSH
        yas::shared_buffer yas_buffer = yas::save<::yas::mem | ::yas::json | ::yas::no_header>(obj);

        if constexpr (is_std_array_v<OutputBlob>)
        {
            constexpr size_t N = array_traits<OutputBlob>::size;
            if (N < yas_buffer.size)
            {
                throw std::runtime_error("Array too small for yas_json serialization");
            }
            OutputBlob result{};
            std::copy(yas_buffer.data.get(), yas_buffer.data.get() + yas_buffer.size, result.begin());
            return result;
        }
        else
        {
            return OutputBlob(yas_buffer.data.get(), yas_buffer.data.get() + yas_buffer.size);
        }
        YAS_WARNINGS_POP
    }

    template<
        class OutputBlob = std::vector<std::uint8_t>,
        typename T>
    OutputBlob to_yas_binary(const T& obj)
    {
        YAS_WARNINGS_PUSH
        yas::shared_buffer yas_buffer = yas::save<::yas::mem | ::yas::binary | ::yas::no_header>(obj);

        if constexpr (is_std_array_v<OutputBlob>)
        {
            constexpr size_t N = array_traits<OutputBlob>::size;
            if (N < yas_buffer.size)
            {
                throw std::runtime_error("Array too small for yas_binary serialization");
            }
            OutputBlob result{};
            std::copy(yas_buffer.data.get(), yas_buffer.data.get() + yas_buffer.size, result.begin());
            return result;
        }
        else
        {
            return OutputBlob(yas_buffer.data.get(), yas_buffer.data.get() + yas_buffer.size);
        }
        YAS_WARNINGS_POP
    }

    template<
        class OutputBlob = std::vector<std::uint8_t>,
        typename T>
    OutputBlob to_compressed_yas_binary(const T& obj)
    {
        YAS_WARNINGS_PUSH
        yas::shared_buffer yas_buffer = yas::save<::yas::mem | ::yas::binary | ::yas::compacted | ::yas::no_header>(obj);

        if constexpr (is_std_array_v<OutputBlob>)
        {
            constexpr size_t N = array_traits<OutputBlob>::size;
            if (N < yas_buffer.size)
            {
                throw std::runtime_error("Array too small for compressed yas_binary serialization");
            }
            OutputBlob result{};
            std::copy(yas_buffer.data.get(), yas_buffer.data.get() + yas_buffer.size, result.begin());
            return result;
        }
        else
        {
            return OutputBlob(yas_buffer.data.get(), yas_buffer.data.get() + yas_buffer.size);
        }
        YAS_WARNINGS_POP
    }

#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
    // protobuf serialization using member function protobuf_serialise
    template<
        class OutputBlob = std::vector<std::uint8_t>,
        typename T>
    OutputBlob to_protobuf(const T& obj)
    {
        std::vector<char> buffer;
        obj.protobuf_serialise(buffer);

        if constexpr (is_std_array_v<OutputBlob>)
        {
            constexpr size_t N = array_traits<OutputBlob>::size;
            if (N < buffer.size())
            {
                throw std::runtime_error("Array too small for protobuf serialization");
            }
            OutputBlob result{};
            std::copy(buffer.begin(), buffer.end(), result.begin());
            return result;
        }
        else
        {
            return OutputBlob(buffer.data(), buffer.data() + buffer.size());
        }
    }
#endif

    template<
        class OutputBlob = std::vector<std::uint8_t>,
        typename T>
    OutputBlob serialise(
        const T& obj,
        encoding enc)
    {
        if (enc == encoding::yas_json)
            return to_yas_json<OutputBlob>(obj);
        if (enc == encoding::yas_binary)
            return to_yas_binary<OutputBlob>(obj);
        if (enc == encoding::yas_compressed_binary)
            return to_compressed_yas_binary<OutputBlob>(obj);
#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
        if (enc == encoding::protocol_buffers)
            return to_protobuf<OutputBlob>(obj);
#endif
        throw std::runtime_error("invalid encoding type");
    }

    template<typename T>
    uint64_t get_saved_size(
        const T& obj,
        encoding enc)
    {
        if (enc == encoding::yas_json)
            return yas_json_saved_size(obj);
        if (enc == encoding::yas_binary)
            return yas_binary_saved_size(obj);
        if (enc == encoding::yas_compressed_binary)
            return compressed_yas_binary_saved_size(obj);
#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
        if (enc == encoding::protocol_buffers)
            return protobuf_saved_size(obj);
#endif
        throw std::runtime_error("invalid encoding type");
    }

    // deserialisation primatives
    template<typename T>
    std::string from_yas_json(
        const byte_span& data,
        T& obj)
    {
        try
        {
            YAS_WARNINGS_PUSH
            yas::load<yas::mem | yas::json | yas::no_header>(
                yas::intrusive_buffer{reinterpret_cast<const char*>(data.data()), data.size()}, obj);
            YAS_WARNINGS_POP
            return "";
        }
        catch (const std::exception& ex)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return std::string(
                       "An exception has occurred a data blob was incompatible with the type that is "
                       "deserialising to: ")
                   + ex.what();
        }
        catch (...)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return "An exception has occurred a data blob was incompatible with the type that is deserialising to";
        }
    }

    template<typename T>
    std::string from_yas_binary(
        const byte_span& data,
        T& obj)
    {
        try
        {
            YAS_WARNINGS_PUSH
            yas::load<yas::mem | ::yas::binary | ::yas::no_header>(
                yas::intrusive_buffer{reinterpret_cast<const char*>(data.data()), data.size()}, obj);
            YAS_WARNINGS_POP
            return "";
        }
        catch (const std::exception& ex)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return std::string(
                       "An exception has occurred a data blob was incompatible with the type that is "
                       "deserialising to: ")
                   + ex.what();
        }
        catch (...)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return "An exception has occurred a data blob was incompatible with the type that is deserialising to";
        }
    }

    template<typename T>
    std::string from_yas_compressed_binary(
        const byte_span& data,
        T& obj)
    {
        try
        {
            YAS_WARNINGS_PUSH
            yas::load<yas::mem | ::yas::binary | ::yas::compacted | ::yas::no_header>(
                yas::intrusive_buffer{reinterpret_cast<const char*>(data.data()), data.size()}, obj);
            YAS_WARNINGS_POP
            return "";
        }
        catch (const std::exception& ex)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return std::string(
                       "An exception has occurred a data blob was incompatible with the type that is "
                       "deserialising to: ")
                   + ex.what();
        }
        catch (...)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return "An exception has occurred a data blob was incompatible with the type that is deserialising to";
        }
    }

#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
    template<typename T>
    std::string from_protobuf(
        const byte_span& data,
        T& obj)
    {
        try
        {
            obj.protobuf_deserialise(
                std::vector<char>(
                    reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + data.size()));
            return "";
        }
        catch (const std::exception& ex)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return std::string(
                       "An exception has occurred a data blob was incompatible with the type that is "
                       "deserialising to: ")
                   + ex.what();
        }
        catch (...)
        {
            // an error has occurred so do the best one can and set the type_id to 0
            return "An exception has occurred a data blob was incompatible with the type that is deserialising to";
        }
    }
#endif

    template<typename T>
    std::string deserialise(
        encoding enc,
        const byte_span& data,
        T& obj)
    {
        if (enc == encoding::yas_json)
            return from_yas_json(data, obj);
        if (enc == encoding::yas_binary)
            return from_yas_binary(data, obj);
        if (enc == encoding::yas_compressed_binary)
            return from_yas_compressed_binary(data, obj);
#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
        if (enc == encoding::protocol_buffers)
            return from_protobuf(data, obj);
#endif
        return "invalid encoding type";
    }
}
