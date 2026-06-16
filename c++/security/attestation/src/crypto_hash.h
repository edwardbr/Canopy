/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace canopy::security::attestation::detail
{
    inline constexpr size_t crypto_sha256_digest_size = 32;

    [[nodiscard]] auto sha256_digest(const std::vector<uint8_t>& data) -> std::optional<std::vector<uint8_t>>;

    [[nodiscard]] auto hmac_sha256(
        std::string_view key,
        const std::vector<uint8_t>& data) -> std::optional<std::vector<uint8_t>>;

    [[nodiscard]] auto constant_time_equal(
        const std::vector<uint8_t>& left,
        const std::vector<uint8_t>& right) noexcept -> bool;
} // namespace canopy::security::attestation::detail
