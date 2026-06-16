/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <security/attestation/types.h>

namespace canopy::security::attestation::detail
{
    inline constexpr size_t sha256_digest_size = 32;

    using sha256_digest = std::array<uint8_t, sha256_digest_size>;

    [[nodiscard]] auto derive_development_shared_secret(const security_context& context)
        -> std::optional<std::vector<uint8_t>>;

    [[nodiscard]] auto derive_session_root_secret(
        const security_context& context,
        const std::vector<uint8_t>& session_secret) -> std::optional<std::vector<uint8_t>>;

    [[nodiscard]] auto derive_protected_rpc_key_material(
        const std::vector<uint8_t>& root_secret,
        const protected_key_scope& scope,
        const security_context& context,
        size_t output_size) -> std::optional<std::vector<uint8_t>>;
} // namespace canopy::security::attestation::detail
