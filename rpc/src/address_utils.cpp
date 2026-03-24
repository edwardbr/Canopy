/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/internal/address_utils.h>

namespace rpc
{
    uint64_t get_bits_le(const std::vector<uint8_t>& data, uint16_t offset, uint16_t width)
    {
        if (width == 0)
            return 0;

        if (width > 64u)
            width = 64u;

        uint64_t value = 0;
        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = offset + i;
            auto byte_index = static_cast<size_t>(bit / 8u);
            if (byte_index >= data.size())
                break;

            auto mask = static_cast<uint8_t>(1u << (bit % 8u));
            if ((data[byte_index] & mask) != 0)
                value |= (uint64_t(1) << i);
        }
        return value;
    }

    bool set_bits_le(std::vector<uint8_t>& data, uint16_t offset, uint16_t width, uint64_t value)
    {
        if (width == 0)
            return value == 0;

        if (width < 64u && value >= (uint64_t(1) << width))
            return false;

        auto required_bits = static_cast<uint32_t>(offset) + static_cast<uint32_t>(width);
        auto required_bytes = static_cast<size_t>((required_bits + 7u) / 8u);
        if (data.size() < required_bytes)
            data.resize(required_bytes, 0);

        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = offset + i;
            auto& byte = data[bit / 8u];
            auto mask = static_cast<uint8_t>(1u << (bit % 8u));
            if (((value >> i) & 1u) != 0)
                byte = static_cast<uint8_t>(byte | mask);
            else
                byte = static_cast<uint8_t>(byte & ~mask);
        }
        return true;
    }

    uint64_t get_bits_be(const std::vector<uint8_t>& data, uint16_t offset, uint16_t width)
    {
        if (width == 0)
            return 0;

        if (width > 64u)
            width = 64u;

        uint64_t value = 0;
        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = offset + i;
            auto byte_index = static_cast<size_t>(bit / 8u);
            auto bit_index = static_cast<uint8_t>(7u - (bit % 8u));
            value <<= 1u;
            value |= static_cast<uint64_t>((data[byte_index] >> bit_index) & 1u);
        }
        return value;
    }

    bool set_bits_be(std::vector<uint8_t>& data, uint16_t offset, uint16_t width, uint64_t value)
    {
        if (width == 0)
            return value == 0;

        if (width < 64u && value >= (uint64_t(1) << width))
            return false;

        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = static_cast<uint16_t>(offset + width - 1u - i);
            auto byte_index = static_cast<size_t>(bit / 8u);
            auto bit_index = static_cast<uint8_t>(7u - (bit % 8u));
            auto mask = static_cast<uint8_t>(1u << bit_index);
            if (((value >> i) & 1u) != 0)
                data[byte_index] = static_cast<uint8_t>(data[byte_index] | mask);
            else
                data[byte_index] = static_cast<uint8_t>(data[byte_index] & ~mask);
        }
        return true;
    }
} // namespace rpc
