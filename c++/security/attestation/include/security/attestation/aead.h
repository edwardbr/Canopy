/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace canopy::security::attestation
{
    inline constexpr size_t aead_aes_256_gcm_tag_size = 16U;

    struct aead_ciphertext
    {
        std::vector<uint8_t> payload;
        std::vector<uint8_t> authentication_tag;
    };

    // Encrypts with AES-256-GCM using OpenSSL/SGXSSL EVP.
    //
    // The implementation is intentionally kept in a separate translation unit
    // so reviewers can audit the AEAD call sequence independently of RPC
    // envelope construction. Canopy derives the key and nonce elsewhere; this
    // helper only enforces the concrete AES-GCM operation and tag handling.
    [[nodiscard]] auto encrypt_aes_256_gcm(
        const aead_key_material& key,
        const std::array<
            uint8_t,
            aead_nonce_size>& nonce,
        const std::vector<uint8_t>& plaintext,
        const std::vector<uint8_t>& aad) -> std::optional<aead_ciphertext>;

    // Decrypts with AES-256-GCM and returns plaintext only after tag
    // verification succeeds for both the ciphertext and the supplied AAD.
    [[nodiscard]] auto decrypt_aes_256_gcm(
        const aead_key_material& key,
        const std::array<
            uint8_t,
            aead_nonce_size>& nonce,
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& authentication_tag,
        const std::vector<uint8_t>& aad) -> std::optional<std::vector<uint8_t>>;
} // namespace canopy::security::attestation
