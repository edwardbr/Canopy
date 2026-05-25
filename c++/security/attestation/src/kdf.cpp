/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "kdf.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/evp.h>
#include <openssl/kdf.h>

#ifndef EVP_PKEY_CTX_set_hkdf_mode
#  define EVP_PKEY_CTX_set_hkdf_mode EVP_PKEY_CTX_hkdf_mode
#endif

namespace canopy::security::attestation::detail
{
    namespace
    {
        // Domain separator for the byte encoding used as HKDF input. This is
        // not the negotiated attestation handshake suite.
        constexpr std::string_view canonical_kdf_encoding_domain = "Canopy-Attestation-v1";
        constexpr std::string_view kdf_purpose_development_shared_secret = "development-shared-secret";
        constexpr std::string_view kdf_purpose_session_root_secret = "session-root-secret";
        constexpr std::string_view kdf_purpose_protected_rpc_aead_key = "protected-rpc-aead-key";
        constexpr std::string_view kdf_salt_purpose_fake_development_kex = "fake-development-kex";
        constexpr size_t hkdf_sha256_max_output = 255 * sha256_digest_size;

        class canonical_kdf_encoder final
        {
        public:
            void append_u16(uint16_t value)
            {
                if (!valid_)
                    return;
                if (!can_append(sizeof(value)))
                {
                    valid_ = false;
                    return;
                }
                bytes_.push_back(static_cast<uint8_t>((value >> 8) & 0xffU));
                bytes_.push_back(static_cast<uint8_t>(value & 0xffU));
            }

            void append_u32(uint32_t value)
            {
                if (!valid_)
                    return;
                if (!can_append(sizeof(value)))
                {
                    valid_ = false;
                    return;
                }
                for (int shift = 24; shift >= 0; shift -= 8)
                    bytes_.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            }

            void append_u64(uint64_t value)
            {
                if (!valid_)
                    return;
                if (!can_append(sizeof(value)))
                {
                    valid_ = false;
                    return;
                }
                for (int shift = 56; shift >= 0; shift -= 8)
                    bytes_.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            }

            void append_bytes(
                const uint8_t* data,
                size_t size)
            {
                if (!valid_)
                    return;
                if (size > std::numeric_limits<uint32_t>::max() || (size > 0 && data == nullptr))
                {
                    valid_ = false;
                    return;
                }
                if (!can_append(sizeof(uint32_t) + size))
                {
                    valid_ = false;
                    return;
                }
                append_u32(static_cast<uint32_t>(size));
                if (size == 0)
                    return;
                bytes_.insert(bytes_.end(), data, data + size);
            }

            void append_string(std::string_view value)
            {
                append_bytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
            }

            void append_identity(const identity& value)
            {
                append_string(value.enclave_id);
                append_string(value.zone_id);
            }

            [[nodiscard]] auto finish() && -> std::optional<std::vector<uint8_t>>
            {
                if (!valid_)
                    return std::nullopt;
                return std::move(bytes_);
            }

        private:
            [[nodiscard]] auto can_append(size_t size) const -> bool
            {
                return size <= bytes_.max_size() && bytes_.size() <= bytes_.max_size() - size;
            }

            std::vector<uint8_t> bytes_;
            bool valid_{true};
        };

        auto checked_int_size(size_t size) -> std::optional<int>
        {
            if (size > static_cast<size_t>(std::numeric_limits<int>::max()))
                return std::nullopt;
            return static_cast<int>(size);
        }

        auto hkdf_extract_sha256(
            const std::vector<uint8_t>& salt,
            const std::vector<uint8_t>& input_keying_material) -> std::optional<sha256_digest>
        {
            auto salt_size = checked_int_size(salt.size());
            auto key_size = checked_int_size(input_keying_material.size());
            if (!salt_size.has_value() || !key_size.has_value())
                return std::nullopt;

            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
                EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
            if (!ctx)
                return std::nullopt;

            if (EVP_PKEY_derive_init(ctx.get()) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set_hkdf_mode(ctx.get(), EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt.empty() ? nullptr : salt.data(), salt_size.value()) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set1_hkdf_key(
                    ctx.get(), input_keying_material.empty() ? nullptr : input_keying_material.data(), key_size.value())
                <= 0)
                return std::nullopt;

            sha256_digest out{};
            size_t out_size = out.size();
            if (EVP_PKEY_derive(ctx.get(), out.data(), &out_size) <= 0)
                return std::nullopt;
            if (out_size != out.size())
                return std::nullopt;
            return out;
        }

        auto hkdf_expand_sha256(
            const std::vector<uint8_t>& pseudo_random_key,
            const std::vector<uint8_t>& info,
            size_t output_size) -> std::optional<std::vector<uint8_t>>
        {
            auto key_size = checked_int_size(pseudo_random_key.size());
            auto info_size = checked_int_size(info.size());
            if (!key_size.has_value() || !info_size.has_value())
                return std::nullopt;
            if (output_size > hkdf_sha256_max_output)
                return std::nullopt;

            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
                EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
            if (!ctx)
                return std::nullopt;

            if (EVP_PKEY_derive_init(ctx.get()) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set_hkdf_mode(ctx.get(), EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0)
                return std::nullopt;
            if (EVP_PKEY_CTX_set1_hkdf_key(
                    ctx.get(), pseudo_random_key.empty() ? nullptr : pseudo_random_key.data(), key_size.value())
                <= 0)
                return std::nullopt;
            if (!info.empty() && EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), info.data(), info_size.value()) <= 0)
                return std::nullopt;

            std::vector<uint8_t> out(output_size);
            size_t out_size = out.size();
            if (EVP_PKEY_derive(ctx.get(), out.data(), &out_size) <= 0)
                return std::nullopt;
            if (out_size != out.size())
                return std::nullopt;
            return out;
        }

        auto bytes_from_digest(const sha256_digest& value) -> std::vector<uint8_t>
        {
            return std::vector<uint8_t>(value.begin(), value.end());
        }

        auto session_participant_key(const identity& value) -> std::string
        {
            if (!value.enclave_id.empty())
                return value.enclave_id;
            return "/" + value.zone_id;
        }

        void append_canonical_enclave_pair(
            canonical_kdf_encoder& encoder,
            const identity& first,
            const identity& second)
        {
            auto left = session_participant_key(first);
            auto right = session_participant_key(second);
            if (right < left)
                std::swap(left, right);
            encoder.append_string(left);
            encoder.append_string(right);
        }

        auto make_labeled_salt(std::string_view label) -> std::optional<std::vector<uint8_t>>
        {
            canonical_kdf_encoder encoder;
            encoder.append_string(canonical_kdf_encoding_domain);
            encoder.append_string(label);
            return std::move(encoder).finish();
        }

        auto make_labeled_info(
            std::string_view label,
            const std::vector<uint8_t>& context,
            size_t output_size) -> std::optional<std::vector<uint8_t>>
        {
            if (output_size > std::numeric_limits<uint16_t>::max())
                return std::nullopt;

            canonical_kdf_encoder encoder;
            encoder.append_u16(static_cast<uint16_t>(output_size));
            encoder.append_string(canonical_kdf_encoding_domain);
            encoder.append_string(label);
            encoder.append_bytes(context.empty() ? nullptr : context.data(), context.size());
            return std::move(encoder).finish();
        }

        auto make_development_shared_secret_context(const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            canonical_kdf_encoder encoder;
            encoder.append_string(canonical_kdf_encoding_domain);
            encoder.append_string(kdf_purpose_development_shared_secret);
            // RPC view: this is the stable security relationship being set up.
            // Multiple zones can share an enclave identity, so the pair is
            // canonicalised by enclave id before key material is derived.
            append_canonical_enclave_pair(encoder, context.local_identity, context.peer_identity);
            // transcript_id separates two different executions of the
            // handshake. session_epoch separates key generations if a route is
            // re-established later.
            encoder.append_u64(context.transcript_id);
            encoder.append_u64(context.session_epoch);
            // These are the attestation result that authorised this session,
            // not message schemas. A production backend would set hardware
            // levels; the fake backend sets development.
            encoder.append_string(context.backend_id);
            encoder.append_string(security_level_name(context.level));
            return std::move(encoder).finish();
        }

        auto make_session_root_context(const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            canonical_kdf_encoder encoder;
            encoder.append_string(canonical_kdf_encoding_domain);
            encoder.append_string(kdf_purpose_session_root_secret);
            encoder.append_string(context.session_id);
            encoder.append_u64(context.transcript_id);
            encoder.append_u64(context.session_epoch);
            append_canonical_enclave_pair(encoder, context.local_identity, context.peer_identity);
            return std::move(encoder).finish();
        }

        auto make_protected_rpc_context(
            const protected_key_scope& scope,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            canonical_kdf_encoder encoder;
            encoder.append_string(canonical_kdf_encoding_domain);
            encoder.append_string(kdf_purpose_protected_rpc_aead_key);
            encoder.append_string(scope.session_id);
            encoder.append_u64(context.session_epoch);
            encoder.append_u64(context.transcript_id);
            encoder.append_identity(scope.caller_identity);
            encoder.append_identity(scope.destination_identity);
            switch (scope.direction)
            {
            case protected_rpc_direction::caller_to_destination:
                encoder.append_string("caller-to-destination");
                break;
            case protected_rpc_direction::destination_to_caller:
                encoder.append_string("destination-to-caller");
                break;
            }
            return std::move(encoder).finish();
        }
    } // namespace

    auto derive_development_shared_secret(const security_context& context) -> std::optional<std::vector<uint8_t>>
    {
        auto salt = make_labeled_salt(kdf_salt_purpose_fake_development_kex);
        auto kdf_context = make_development_shared_secret_context(context);
        if (!salt.has_value() || !kdf_context.has_value())
            return std::nullopt;

        auto digest = hkdf_extract_sha256(salt.value(), kdf_context.value());
        if (!digest.has_value())
            return std::nullopt;
        return bytes_from_digest(digest.value());
    }

    auto derive_session_root_secret(
        const security_context& context,
        const std::vector<uint8_t>& session_secret) -> std::optional<std::vector<uint8_t>>
    {
        auto kdf_context = make_session_root_context(context);
        if (!kdf_context.has_value())
            return std::nullopt;

        auto digest = hkdf_extract_sha256(kdf_context.value(), session_secret);
        if (!digest.has_value())
            return std::nullopt;
        return bytes_from_digest(digest.value());
    }

    auto derive_protected_rpc_key_material(
        const std::vector<uint8_t>& root_secret,
        const protected_key_scope& scope,
        const security_context& context,
        size_t output_size) -> std::optional<std::vector<uint8_t>>
    {
        auto kdf_context = make_protected_rpc_context(scope, context);
        if (!kdf_context.has_value())
            return std::nullopt;

        auto info = make_labeled_info(kdf_purpose_protected_rpc_aead_key, kdf_context.value(), output_size);
        if (!info.has_value())
            return std::nullopt;
        return hkdf_expand_sha256(root_secret, info.value(), output_size);
    }
} // namespace canopy::security::attestation::detail
