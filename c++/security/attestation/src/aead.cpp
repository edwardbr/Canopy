/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/aead.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace canopy::security::attestation
{
    namespace
    {
        inline constexpr int openssl_success = 1;

        [[nodiscard]] auto fits_openssl_int(size_t value) noexcept -> bool
        {
            return value <= static_cast<size_t>(std::numeric_limits<int>::max());
        }

        [[nodiscard]] auto openssl_ok(int result) noexcept -> bool
        {
            return result == openssl_success;
        }

        [[nodiscard]] auto openssl_len_is_valid(int len) noexcept -> bool
        {
            return len >= 0;
        }
    } // namespace

    auto encrypt_aes_256_gcm(
        const aead_key_material& key,
        const std::array<
            uint8_t,
            aead_nonce_size>& nonce,
        const std::vector<uint8_t>& plaintext,
        const std::vector<uint8_t>& aad) -> std::optional<aead_ciphertext>
    {
        if (!fits_openssl_int(plaintext.size()) || !fits_openssl_int(aad.size()))
            return std::nullopt;

        // This is not a Canopy AES implementation. The primitive is OpenSSL
        // EVP_aes_256_gcm().
        //
        // Corroborating references for the required call sequence and security
        // model:
        // - NIST SP 800-38D, Recommendation for GCM and GMAC:
        //   https://doi.org/10.6028/NIST.SP.800-38D
        // - RFC 5116, Authenticated Encryption with Associated Data:
        //   https://www.rfc-editor.org/rfc/rfc5116
        // - OpenSSL EVP Authenticated Encryption and Decryption:
        //   https://docs.openssl.org/3.0/man3/EVP_EncryptInit/
        //
        // The OpenSSL GCM order is:
        // 1. Select EVP_aes_256_gcm().
        // 2. Set the 96-bit nonce/IV length before providing key material.
        // 3. Provide key and nonce.
        // 4. Feed AAD with a null output buffer so it is authenticated but not encrypted.
        // 5. Encrypt the payload.
        // 6. Finalise, then read the fixed 128-bit authentication tag.
        using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
        ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!ctx)
            return std::nullopt;

        if (!openssl_ok(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr)))
            return std::nullopt;
        if (!openssl_ok(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr)))
            return std::nullopt;
        if (!openssl_ok(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.key.data(), nonce.data())))
            return std::nullopt;

        int len = 0;
        if (!aad.empty()
            && !openssl_ok(EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size()))))
        {
            return std::nullopt;
        }

        aead_ciphertext result;
        result.payload.resize(plaintext.size());
        if (!plaintext.empty()
            && !openssl_ok(EVP_EncryptUpdate(
                ctx.get(), result.payload.data(), &len, plaintext.data(), static_cast<int>(plaintext.size()))))
        {
            return std::nullopt;
        }
        if (!openssl_len_is_valid(len))
            return std::nullopt;
        size_t produced = static_cast<size_t>(len);
        if (produced > result.payload.size())
            return std::nullopt;

        std::array<uint8_t, EVP_MAX_BLOCK_LENGTH> final_block{};
        if (!openssl_ok(EVP_EncryptFinal_ex(ctx.get(), final_block.data(), &len)))
            return std::nullopt;
        if (!openssl_len_is_valid(len))
            return std::nullopt;
        if (static_cast<size_t>(len) > final_block.size())
            return std::nullopt;
        if (static_cast<size_t>(len) > result.payload.size() - produced)
            return std::nullopt;
        if (len > 0)
        {
            const auto output_offset = static_cast<decltype(result.payload)::difference_type>(produced);
            std::copy(final_block.begin(), final_block.begin() + len, result.payload.begin() + output_offset);
        }
        produced += static_cast<size_t>(len);
        result.payload.resize(produced);

        result.authentication_tag.resize(aead_aes_256_gcm_tag_size);
        if (!openssl_ok(EVP_CIPHER_CTX_ctrl(
                ctx.get(),
                EVP_CTRL_GCM_GET_TAG,
                static_cast<int>(result.authentication_tag.size()),
                result.authentication_tag.data())))
        {
            return std::nullopt;
        }
        return result;
    }

    auto decrypt_aes_256_gcm(
        const aead_key_material& key,
        const std::array<
            uint8_t,
            aead_nonce_size>& nonce,
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& authentication_tag,
        const std::vector<uint8_t>& aad) -> std::optional<std::vector<uint8_t>>
    {
        if (authentication_tag.size() != aead_aes_256_gcm_tag_size)
            return std::nullopt;
        if (!fits_openssl_int(ciphertext.size()) || !fits_openssl_int(aad.size()))
            return std::nullopt;

        // Decryption mirrors encryption, except the tag is installed before
        // EVP_DecryptFinal_ex(). OpenSSL returns success from that final call
        // only when the ciphertext and every AAD byte match the tag.
        using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
        ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!ctx)
            return std::nullopt;

        if (!openssl_ok(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr)))
            return std::nullopt;
        if (!openssl_ok(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr)))
            return std::nullopt;
        if (!openssl_ok(EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.key.data(), nonce.data())))
            return std::nullopt;

        int len = 0;
        if (!aad.empty()
            && !openssl_ok(EVP_DecryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size()))))
        {
            return std::nullopt;
        }

        std::vector<uint8_t> plaintext(ciphertext.size());
        if (!ciphertext.empty()
            && !openssl_ok(EVP_DecryptUpdate(
                ctx.get(), plaintext.data(), &len, ciphertext.data(), static_cast<int>(ciphertext.size()))))
        {
            return std::nullopt;
        }
        if (!openssl_len_is_valid(len))
            return std::nullopt;
        size_t produced = static_cast<size_t>(len);
        if (produced > plaintext.size())
            return std::nullopt;

        std::array<uint8_t, aead_aes_256_gcm_tag_size> expected_tag{};
        std::copy(authentication_tag.begin(), authentication_tag.end(), expected_tag.begin());
        if (!openssl_ok(EVP_CIPHER_CTX_ctrl(
                ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(expected_tag.size()), expected_tag.data())))
        {
            return std::nullopt;
        }

        std::array<uint8_t, EVP_MAX_BLOCK_LENGTH> final_block{};
        if (!openssl_ok(EVP_DecryptFinal_ex(ctx.get(), final_block.data(), &len)))
            return std::nullopt;
        if (!openssl_len_is_valid(len))
            return std::nullopt;
        if (static_cast<size_t>(len) > final_block.size())
            return std::nullopt;
        if (static_cast<size_t>(len) > plaintext.size() - produced)
            return std::nullopt;
        if (len > 0)
        {
            const auto output_offset = static_cast<decltype(plaintext)::difference_type>(produced);
            std::copy(final_block.begin(), final_block.begin() + len, plaintext.begin() + output_offset);
        }
        produced += static_cast<size_t>(len);
        plaintext.resize(produced);
        return plaintext;
    }
} // namespace canopy::security::attestation
