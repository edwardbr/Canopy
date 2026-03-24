/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace rpc
{
    // Low-level bit-packing helpers operating on byte vectors.
    // Bits are numbered from the least-significant end of each byte (LE) or the
    // most-significant end (BE). Both helpers handle widths up to 64 bits.
    uint64_t get_bits_le(const std::vector<uint8_t>& data, uint16_t offset, uint16_t width);
    bool set_bits_le(std::vector<uint8_t>& data, uint16_t offset, uint16_t width, uint64_t value);
    uint64_t get_bits_be(const std::vector<uint8_t>& data, uint16_t offset, uint16_t width);
    bool set_bits_be(std::vector<uint8_t>& data, uint16_t offset, uint16_t width, uint64_t value);
} // namespace rpc
