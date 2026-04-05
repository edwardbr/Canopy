/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <rpc/rpc_types.h>

namespace rpc
{
    // Convert between zone_address (blob) and zone_address_args (structured fields).
    // These are lossless except that validation_bits cannot be extracted from a
    // zone_address blob, so to_zone_address_args always returns an empty validation_bits.
    zone_address_args to_zone_address_args(const zone_address& addr);
    zone_address to_zone_address(const zone_address_args& args);

    // Low-level bit-packing helpers operating on byte vectors.
    // Bits are numbered from the least-significant end of each byte (LE) or the
    // most-significant end (BE). Both helpers handle widths up to 64 bits.
    uint64_t get_bits_le(
        const std::vector<uint8_t>& data,
        uint16_t offset,
        uint16_t width);
    bool set_bits_le(
        std::vector<uint8_t>& data,
        uint16_t offset,
        uint16_t width,
        uint64_t value);
    uint64_t get_bits_be(
        const std::vector<uint8_t>& data,
        uint16_t offset,
        uint16_t width);
    bool set_bits_be(
        std::vector<uint8_t>& data,
        uint16_t offset,
        uint16_t width,
        uint64_t value);
} // namespace rpc
