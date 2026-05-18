/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "crypto_hash.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <limits>

namespace canopy::security::attestation::detail
{
    auto sha256_digest(const std::vector<uint8_t>& data) -> std::optional<std::vector<uint8_t>>
    {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        const auto* input = data.empty() ? nullptr : data.data();
        if (EVP_Digest(input, data.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) != 1)
            return std::nullopt;
        if (digest_size != crypto_sha256_digest_size)
            return std::nullopt;
        return std::vector<uint8_t>(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(digest_size));
    }

    auto hmac_sha256(
        std::string_view key,
        const std::vector<uint8_t>& data) -> std::optional<std::vector<uint8_t>>
    {
        if (key.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            return std::nullopt;

        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        const auto* input = data.empty() ? nullptr : data.data();
        auto* result = HMAC(
            EVP_sha256(), key.data(), static_cast<int>(key.size()), input, data.size(), digest.data(), &digest_size);
        if (!result || digest_size != crypto_sha256_digest_size)
            return std::nullopt;
        return std::vector<uint8_t>(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(digest_size));
    }

    auto constant_time_equal(
        const std::vector<uint8_t>& left,
        const std::vector<uint8_t>& right) noexcept -> bool
    {
        if (left.size() != right.size() || left.empty())
            return false;

        uint8_t difference = 0;
        for (size_t index = 0; index < left.size(); ++index)
            difference = static_cast<uint8_t>(difference | (left[index] ^ right[index]));
        return difference == 0;
    }
} // namespace canopy::security::attestation::detail
