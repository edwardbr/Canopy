/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/protected_rpc.h>

#include <openssl/evp.h>

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>

namespace canopy::security::attestation
{
    namespace
    {
        inline constexpr uint32_t protected_rpc_plaintext_magic = 0x43505231U; // "CPR1"
        inline constexpr size_t bytes_per_kib = 1024U;
        inline constexpr size_t kib_per_mib = 1024U;
        inline constexpr size_t max_protected_field_size_mib = 64U;
        inline constexpr size_t max_protected_field_size = max_protected_field_size_mib * kib_per_mib * bytes_per_kib;
        inline constexpr size_t max_public_back_channel_entries = 4096U;
        inline constexpr size_t aes_gcm_tag_size_bytes = 16U;
        inline constexpr int openssl_success = 1;
        inline constexpr int lowest_shift_bits = 0;
        inline constexpr int bits_per_byte = 8;
        inline constexpr int uint32_high_shift_bits = 24;
        inline constexpr int uint64_high_shift_bits = 56;
        inline constexpr uint32_t byte_mask = 0xffU;

        template<typename T>
        auto rejected(
            int error_code,
            std::string reason) -> protected_rpc_result<T>
        {
            protected_rpc_result<T> result;
            result.accepted = false;
            result.error.error_code = error_code;
            result.error.reason = std::move(reason);
            return result;
        }

        template<typename T> auto accepted(T value) -> protected_rpc_result<T>
        {
            protected_rpc_result<T> result;
            result.accepted = true;
            result.error.error_code = rpc::error::OK();
            result.value = std::move(value);
            return result;
        }

        auto can_append(
            const std::vector<uint8_t>& out,
            size_t size) noexcept -> bool
        {
            return size <= out.max_size() - out.size();
        }

        auto has_remaining(
            const std::vector<uint8_t>& in,
            size_t offset,
            size_t size) noexcept -> bool
        {
            return offset <= in.size() && size <= in.size() - offset;
        }

        auto fits_u32(size_t value) noexcept -> bool
        {
            return value <= static_cast<size_t>(std::numeric_limits<uint32_t>::max());
        }

        auto fits_int(size_t value) noexcept -> bool
        {
            return value <= static_cast<size_t>(std::numeric_limits<int>::max());
        }

        auto openssl_ok(int result) noexcept -> bool
        {
            return result == openssl_success;
        }

        class writer
        {
        public:
            [[nodiscard]] auto ok() const noexcept -> bool { return ok_; }
            [[nodiscard]] auto take() -> std::vector<uint8_t> { return std::move(out_); }

            auto append_u8(uint8_t value) -> bool
            {
                if (!can_append(out_, sizeof(value)))
                    return fail();
                out_.push_back(value);
                return true;
            }

            auto append_u32(uint32_t value) -> bool
            {
                if (!can_append(out_, sizeof(value)))
                    return fail();
                for (int shift = uint32_high_shift_bits; shift >= lowest_shift_bits; shift -= bits_per_byte)
                    out_.push_back(static_cast<uint8_t>((value >> shift) & byte_mask));
                return true;
            }

            auto append_u64(uint64_t value) -> bool
            {
                if (!can_append(out_, sizeof(value)))
                    return fail();
                for (int shift = uint64_high_shift_bits; shift >= lowest_shift_bits; shift -= bits_per_byte)
                    out_.push_back(static_cast<uint8_t>((value >> shift) & byte_mask));
                return true;
            }

            auto append_i64(int64_t value) -> bool { return append_u64(static_cast<uint64_t>(value)); }

            auto append_bytes(const std::vector<uint8_t>& value) -> bool
            {
                if (value.size() > max_protected_field_size || !fits_u32(value.size()))
                    return fail();
                if (!can_append(out_, sizeof(uint32_t) + value.size()))
                    return fail();
                if (!append_u32(static_cast<uint32_t>(value.size())))
                    return false;
                out_.insert(out_.end(), value.begin(), value.end());
                return true;
            }

            auto append_chars(const std::vector<char>& value) -> bool
            {
                if (value.size() > max_protected_field_size || !fits_u32(value.size()))
                    return fail();
                if (!can_append(out_, sizeof(uint32_t) + value.size()))
                    return fail();
                if (!append_u32(static_cast<uint32_t>(value.size())))
                    return false;
                out_.insert(out_.end(), value.begin(), value.end());
                return true;
            }

            auto append_string(const std::string& value) -> bool
            {
                if (value.size() > max_protected_field_size || !fits_u32(value.size()))
                    return fail();
                if (!can_append(out_, sizeof(uint32_t) + value.size()))
                    return fail();
                if (!append_u32(static_cast<uint32_t>(value.size())))
                    return false;
                out_.insert(out_.end(), value.begin(), value.end());
                return true;
            }

            auto append_back_channel(const std::vector<rpc::back_channel_entry>& entries) -> bool
            {
                if (entries.size() > max_public_back_channel_entries || !fits_u32(entries.size()))
                    return fail();
                if (!append_u32(static_cast<uint32_t>(entries.size())))
                    return false;
                for (const auto& entry : entries)
                {
                    if (!append_u64(entry.type_id))
                        return false;
                    if (!append_bytes(entry.payload))
                        return false;
                }
                return true;
            }

        private:
            auto fail() noexcept -> bool
            {
                ok_ = false;
                return false;
            }

            std::vector<uint8_t> out_;
            bool ok_{true};
        };

        class reader
        {
        public:
            explicit reader(const std::vector<uint8_t>& in)
                : in_(in)
            {
            }

            [[nodiscard]] auto done() const noexcept -> bool { return ok_ && offset_ == in_.size(); }
            [[nodiscard]] auto ok() const noexcept -> bool { return ok_; }

            auto read_u8(uint8_t& value) -> bool
            {
                if (!has_remaining(in_, offset_, sizeof(value)))
                    return fail();
                value = in_[offset_++];
                return true;
            }

            auto read_u32(uint32_t& value) -> bool
            {
                if (!has_remaining(in_, offset_, sizeof(value)))
                    return fail();
                value = 0;
                for (int shift = uint32_high_shift_bits; shift >= lowest_shift_bits; shift -= bits_per_byte)
                {
                    value |= static_cast<uint32_t>(in_[offset_++]) << shift;
                }
                return true;
            }

            auto read_u64(uint64_t& value) -> bool
            {
                if (!has_remaining(in_, offset_, sizeof(value)))
                    return fail();
                value = 0;
                for (int shift = uint64_high_shift_bits; shift >= lowest_shift_bits; shift -= bits_per_byte)
                {
                    value |= static_cast<uint64_t>(in_[offset_++]) << shift;
                }
                return true;
            }

            auto read_i64(int64_t& value) -> bool
            {
                uint64_t raw = 0;
                if (!read_u64(raw))
                    return false;
                value = static_cast<int64_t>(raw);
                return true;
            }

            auto read_bytes(std::vector<uint8_t>& value) -> bool
            {
                uint32_t size = 0;
                if (!read_u32(size))
                    return false;
                if (size > max_protected_field_size || !has_remaining(in_, offset_, size))
                    return fail();
                value.assign(
                    in_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    in_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
                offset_ += size;
                return true;
            }

            auto read_chars(std::vector<char>& value) -> bool
            {
                std::vector<uint8_t> bytes;
                if (!read_bytes(bytes))
                    return false;
                value.assign(bytes.begin(), bytes.end());
                return true;
            }

            auto read_string(std::string& value) -> bool
            {
                std::vector<uint8_t> bytes;
                if (!read_bytes(bytes))
                    return false;
                value.assign(bytes.begin(), bytes.end());
                return true;
            }

            auto read_back_channel(std::vector<rpc::back_channel_entry>& entries) -> bool
            {
                uint32_t count = 0;
                if (!read_u32(count))
                    return false;
                if (count > max_public_back_channel_entries)
                    return fail();
                entries.clear();
                entries.reserve(count);
                for (uint32_t index = 0; index < count; ++index)
                {
                    rpc::back_channel_entry entry;
                    if (!read_u64(entry.type_id))
                        return false;
                    if (!read_bytes(entry.payload))
                        return false;
                    entries.push_back(std::move(entry));
                }
                return true;
            }

        private:
            auto fail() noexcept -> bool
            {
                ok_ = false;
                return false;
            }

            const std::vector<uint8_t>& in_;
            size_t offset_{0};
            bool ok_{true};
        };

        auto zone_blob(const rpc::zone& zone) -> const std::vector<uint8_t>&
        {
            return zone.get_address().get_blob();
        }

        auto remote_object_blob(const rpc::remote_object& object) -> const std::vector<uint8_t>&
        {
            return object.get_address().get_blob();
        }

        auto append_envelope_metadata(
            writer& out,
            const rpc::encrypted_payload& envelope) -> bool
        {
            return out.append_string(envelope.session_id) && out.append_u64(envelope.session_epoch)
                   && out.append_u64(envelope.e2e_counter);
        }

        auto append_send_outer_aad(
            writer& out,
            const rpc::send_params& outer,
            const rpc::encrypted_payload& envelope,
            protected_rpc_kind kind) -> bool
        {
            return out.append_u32(protected_rpc_plaintext_magic) && out.append_u32(protected_rpc_envelope_version)
                   && out.append_u8(static_cast<uint8_t>(kind)) && out.append_u64(outer.protocol_version)
                   && out.append_u64(static_cast<uint64_t>(outer.encoding_type)) && out.append_u64(outer.tag)
                   && out.append_bytes(zone_blob(outer.caller_zone_id))
                   && out.append_bytes(remote_object_blob(outer.remote_object_id))
                   && out.append_u64(outer.interface_id.get_val()) && out.append_u64(outer.method_id.get_val())
                   && out.append_u64(outer.request_id) && out.append_back_channel(outer.in_back_channel)
                   && append_envelope_metadata(out, envelope);
        }

        auto append_post_outer_aad(
            writer& out,
            const rpc::post_params& outer,
            const rpc::encrypted_payload& envelope) -> bool
        {
            return out.append_u32(protected_rpc_plaintext_magic) && out.append_u32(protected_rpc_envelope_version)
                   && out.append_u8(static_cast<uint8_t>(protected_rpc_kind::post))
                   && out.append_u64(outer.protocol_version) && out.append_u64(static_cast<uint64_t>(outer.encoding_type))
                   && out.append_u64(outer.tag) && out.append_bytes(zone_blob(outer.caller_zone_id))
                   && out.append_bytes(remote_object_blob(outer.remote_object_id))
                   && out.append_u64(outer.interface_id.get_val()) && out.append_u64(outer.method_id.get_val())
                   && out.append_back_channel(outer.in_back_channel) && append_envelope_metadata(out, envelope);
        }

        auto make_send_request_aad(
            const rpc::send_params& outer,
            const rpc::encrypted_payload& envelope,
            protected_rpc_kind kind) -> std::optional<std::vector<uint8_t>>
        {
            writer out;
            if (!append_send_outer_aad(out, outer, envelope, kind) || !out.ok())
                return std::nullopt;
            return out.take();
        }

        auto make_post_request_aad(
            const rpc::post_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            writer out;
            if (!append_post_outer_aad(out, outer, envelope) || !out.ok())
                return std::nullopt;
            return out.take();
        }

        auto make_response_aad(
            const rpc::send_params& outer_request,
            const rpc::encrypted_payload& envelope,
            uint64_t request_counter) -> std::optional<std::vector<uint8_t>>
        {
            writer out;
            if (!append_send_outer_aad(out, outer_request, envelope, protected_rpc_kind::response))
                return std::nullopt;
            if (!out.append_u64(request_counter) || !out.ok())
                return std::nullopt;
            return out.take();
        }

        auto append_request_common(
            writer& out,
            protected_rpc_kind kind,
            uint64_t protocol_version,
            rpc::encoding encoding_type,
            uint64_t tag,
            const rpc::caller_zone& caller_zone_id,
            const rpc::remote_object& remote_object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const std::vector<char>& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            uint64_t transport_request_id,
            uint64_t service_request_id,
            uint64_t e2e_counter,
            const std::string& session_id,
            uint64_t session_epoch) -> bool
        {
            return out.append_u32(protected_rpc_plaintext_magic) && out.append_u32(protected_rpc_envelope_version)
                   && out.append_u8(static_cast<uint8_t>(kind)) && out.append_u64(protocol_version)
                   && out.append_u64(static_cast<uint64_t>(encoding_type)) && out.append_u64(tag)
                   && out.append_bytes(zone_blob(caller_zone_id))
                   && out.append_bytes(remote_object_blob(remote_object_id)) && out.append_u64(interface_id.get_val())
                   && out.append_u64(method_id.get_val()) && out.append_chars(in_data)
                   && out.append_back_channel(in_back_channel) && out.append_u64(transport_request_id)
                   && out.append_u64(service_request_id) && out.append_u64(e2e_counter) && out.append_string(session_id)
                   && out.append_u64(session_epoch);
        }

        auto encode_send_request_plaintext(
            const rpc::send_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            writer out;
            if (!append_request_common(
                    out,
                    protected_rpc_kind::send,
                    params.protocol_version,
                    params.encoding_type,
                    params.tag,
                    params.caller_zone_id,
                    params.remote_object_id,
                    params.interface_id,
                    params.method_id,
                    params.in_data,
                    params.in_back_channel,
                    params.request_id,
                    protected_rpc_unset_service_request_id,
                    e2e_counter,
                    context.session_id,
                    context.session_epoch)
                || !out.ok())
            {
                return std::nullopt;
            }
            return out.take();
        }

        auto encode_post_request_plaintext(
            const rpc::post_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            writer out;
            if (!append_request_common(
                    out,
                    protected_rpc_kind::post,
                    params.protocol_version,
                    params.encoding_type,
                    params.tag,
                    params.caller_zone_id,
                    params.remote_object_id,
                    params.interface_id,
                    params.method_id,
                    params.in_data,
                    params.in_back_channel,
                    protected_rpc_unset_transport_request_id,
                    protected_rpc_unset_service_request_id,
                    e2e_counter,
                    context.session_id,
                    context.session_epoch)
                || !out.ok())
            {
                return std::nullopt;
            }
            return out.take();
        }

        struct decoded_request
        {
            protected_rpc_kind kind{protected_rpc_kind::send};
            uint64_t transport_request_id{0};
            uint64_t service_request_id{0};
            uint64_t e2e_counter{0};
            std::string session_id;
            uint64_t session_epoch{0};
            rpc::send_params send;
            rpc::post_params post;
        };

        auto read_zone(std::vector<uint8_t> blob) -> std::optional<rpc::zone>
        {
            auto addr = rpc::zone_address::from_blob(std::move(blob));
            if (!addr)
                return std::nullopt;
            return rpc::zone(*addr);
        }

        auto read_remote_object(std::vector<uint8_t> blob) -> std::optional<rpc::remote_object>
        {
            auto addr = rpc::zone_address::from_blob(std::move(blob));
            if (!addr)
                return std::nullopt;
            return rpc::remote_object(*addr);
        }

        auto decode_request_plaintext(
            const std::vector<uint8_t>& plaintext,
            protected_rpc_kind expected_kind) -> std::optional<decoded_request>
        {
            reader in(plaintext);
            uint32_t magic = 0;
            uint32_t version = 0;
            uint8_t raw_kind = 0;
            uint64_t raw_encoding = 0;
            uint64_t raw_interface = 0;
            uint64_t raw_method = 0;
            std::vector<uint8_t> caller_blob_value;
            std::vector<uint8_t> remote_blob_value;

            decoded_request result;
            if (!in.read_u32(magic) || !in.read_u32(version) || !in.read_u8(raw_kind))
                return std::nullopt;
            if (magic != protected_rpc_plaintext_magic || version != protected_rpc_envelope_version)
                return std::nullopt;
            result.kind = static_cast<protected_rpc_kind>(raw_kind);
            if (result.kind != expected_kind)
                return std::nullopt;

            uint64_t protocol_version = 0;
            uint64_t tag = 0;
            if (!in.read_u64(protocol_version) || !in.read_u64(raw_encoding) || !in.read_u64(tag)
                || !in.read_bytes(caller_blob_value) || !in.read_bytes(remote_blob_value) || !in.read_u64(raw_interface)
                || !in.read_u64(raw_method))
            {
                return std::nullopt;
            }

            auto caller_zone_value = read_zone(std::move(caller_blob_value));
            auto remote_object_value = read_remote_object(std::move(remote_blob_value));
            if (!caller_zone_value || !remote_object_value)
                return std::nullopt;

            const auto encoding_type = static_cast<rpc::encoding>(raw_encoding);
            const auto interface_id = rpc::interface_ordinal(raw_interface);
            const auto method_id = rpc::method(raw_method);

            if (expected_kind == protected_rpc_kind::send)
            {
                result.send.protocol_version = protocol_version;
                result.send.encoding_type = encoding_type;
                result.send.tag = tag;
                result.send.caller_zone_id = *caller_zone_value;
                result.send.remote_object_id = *remote_object_value;
                result.send.interface_id = interface_id;
                result.send.method_id = method_id;
                if (!in.read_chars(result.send.in_data) || !in.read_back_channel(result.send.in_back_channel))
                    return std::nullopt;
            }
            else
            {
                result.post.protocol_version = protocol_version;
                result.post.encoding_type = encoding_type;
                result.post.tag = tag;
                result.post.caller_zone_id = *caller_zone_value;
                result.post.remote_object_id = *remote_object_value;
                result.post.interface_id = interface_id;
                result.post.method_id = method_id;
                if (!in.read_chars(result.post.in_data) || !in.read_back_channel(result.post.in_back_channel))
                    return std::nullopt;
            }

            if (!in.read_u64(result.transport_request_id) || !in.read_u64(result.service_request_id)
                || !in.read_u64(result.e2e_counter) || !in.read_string(result.session_id)
                || !in.read_u64(result.session_epoch))
            {
                return std::nullopt;
            }
            if (!in.done())
                return std::nullopt;
            if (expected_kind == protected_rpc_kind::send)
                result.send.request_id = result.transport_request_id;
            return result;
        }

        auto encode_response_plaintext(
            const rpc::send_result& response,
            uint64_t request_counter,
            uint64_t response_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            writer out;
            if (!out.append_u32(protected_rpc_plaintext_magic) || !out.append_u32(protected_rpc_envelope_version)
                || !out.append_u8(static_cast<uint8_t>(protected_rpc_kind::response))
                || !out.append_i64(static_cast<int64_t>(response.error_code)) || !out.append_chars(response.out_buf)
                || !out.append_back_channel(response.out_back_channel) || !out.append_u64(request_counter)
                || !out.append_u64(response_counter) || !out.append_string(context.session_id)
                || !out.append_u64(context.session_epoch) || !out.ok())
            {
                return std::nullopt;
            }
            return out.take();
        }

        struct decoded_response
        {
            rpc::send_result response;
            uint64_t request_counter{0};
            uint64_t response_counter{0};
            std::string session_id;
            uint64_t session_epoch{0};
        };

        auto decode_response_plaintext(const std::vector<uint8_t>& plaintext) -> std::optional<decoded_response>
        {
            reader in(plaintext);
            uint32_t magic = 0;
            uint32_t version = 0;
            uint8_t raw_kind = 0;
            int64_t error_code = 0;
            decoded_response result;
            if (!in.read_u32(magic) || !in.read_u32(version) || !in.read_u8(raw_kind))
                return std::nullopt;
            if (magic != protected_rpc_plaintext_magic || version != protected_rpc_envelope_version
                || static_cast<protected_rpc_kind>(raw_kind) != protected_rpc_kind::response)
            {
                return std::nullopt;
            }
            if (!in.read_i64(error_code) || !in.read_chars(result.response.out_buf)
                || !in.read_back_channel(result.response.out_back_channel) || !in.read_u64(result.request_counter)
                || !in.read_u64(result.response_counter) || !in.read_string(result.session_id)
                || !in.read_u64(result.session_epoch) || !in.done())
            {
                return std::nullopt;
            }
            if (error_code < std::numeric_limits<int>::min() || error_code > std::numeric_limits<int>::max())
                return std::nullopt;
            result.response.error_code = static_cast<int>(error_code);
            return result;
        }

        auto make_nonce(
            const attestation_service& service,
            const aead_key_material& key,
            uint64_t counter)
            -> std::array<
                uint8_t,
                aead_nonce_size>
        {
            return service.make_aead_nonce(key, counter);
        }

        auto gcm_encrypt(
            const aead_key_material& key,
            const std::array<
                uint8_t,
                aead_nonce_size>& nonce,
            const std::vector<uint8_t>& plaintext,
            const std::vector<uint8_t>& aad,
            rpc::encrypted_payload& envelope) -> bool
        {
            if (!fits_int(plaintext.size()) || !fits_int(aad.size()))
                return false;

            using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
            ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
            if (!ctx)
                return false;

            if (!openssl_ok(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr)))
                return false;
            if (!openssl_ok(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr)))
                return false;
            if (!openssl_ok(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.key.data(), nonce.data())))
                return false;

            int len = 0;
            if (!aad.empty()
                && !openssl_ok(EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size()))))
            {
                return false;
            }

            envelope.payload.resize(plaintext.size());
            if (!plaintext.empty()
                && !openssl_ok(EVP_EncryptUpdate(
                    ctx.get(), envelope.payload.data(), &len, plaintext.data(), static_cast<int>(plaintext.size()))))
            {
                return false;
            }
            size_t produced = static_cast<size_t>(len);

            if (!openssl_ok(EVP_EncryptFinal_ex(ctx.get(), envelope.payload.data() + produced, &len)))
                return false;
            produced += static_cast<size_t>(len);
            envelope.payload.resize(produced);

            envelope.authentication_tag.resize(aes_gcm_tag_size_bytes);
            if (!openssl_ok(EVP_CIPHER_CTX_ctrl(
                    ctx.get(),
                    EVP_CTRL_GCM_GET_TAG,
                    static_cast<int>(envelope.authentication_tag.size()),
                    envelope.authentication_tag.data())))
            {
                return false;
            }
            return true;
        }

        auto gcm_decrypt(
            const aead_key_material& key,
            const std::array<
                uint8_t,
                aead_nonce_size>& nonce,
            const rpc::encrypted_payload& envelope,
            const std::vector<uint8_t>& aad) -> std::optional<std::vector<uint8_t>>
        {
            if (envelope.authentication_tag.size() != aes_gcm_tag_size_bytes)
                return std::nullopt;
            if (!fits_int(envelope.payload.size()) || !fits_int(aad.size()))
                return std::nullopt;

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

            std::vector<uint8_t> plaintext(envelope.payload.size());
            if (!envelope.payload.empty()
                && !openssl_ok(EVP_DecryptUpdate(
                    ctx.get(), plaintext.data(), &len, envelope.payload.data(), static_cast<int>(envelope.payload.size()))))
            {
                return std::nullopt;
            }
            size_t produced = static_cast<size_t>(len);

            if (!openssl_ok(EVP_CIPHER_CTX_ctrl(
                    ctx.get(),
                    EVP_CTRL_GCM_SET_TAG,
                    static_cast<int>(envelope.authentication_tag.size()),
                    const_cast<uint8_t*>(envelope.authentication_tag.data()))))
            {
                return std::nullopt;
            }

            if (!openssl_ok(EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + produced, &len)))
                return std::nullopt;
            produced += static_cast<size_t>(len);
            plaintext.resize(produced);
            return plaintext;
        }

        auto serialise_envelope(const rpc::encrypted_payload& envelope) -> std::optional<std::vector<char>>
        {
            try
            {
                return rpc::serialise<std::vector<char>>(envelope, rpc::encoding::yas_binary);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        auto deserialise_envelope(const std::vector<char>& bytes) -> std::optional<rpc::encrypted_payload>
        {
            rpc::encrypted_payload envelope;
            if (!rpc::deserialise(rpc::encoding::yas_binary, rpc::byte_span(bytes), envelope).empty())
                return std::nullopt;
            if (envelope.session_id.empty() || envelope.session_epoch == protected_rpc_invalid_counter
                || envelope.e2e_counter == protected_rpc_invalid_counter || envelope.payload.size() > max_protected_field_size
                || envelope.authentication_tag.size() != aes_gcm_tag_size_bytes)
            {
                return std::nullopt;
            }
            return envelope;
        }

        auto make_request_scope(
            const security_context& context,
            bool local_is_caller,
            protected_rpc_direction direction) -> protected_key_scope
        {
            protected_key_scope scope;
            scope.session_id = context.session_id;
            scope.caller_identity = local_is_caller ? context.local_identity : context.peer_identity;
            scope.destination_identity = local_is_caller ? context.peer_identity : context.local_identity;
            scope.direction = direction;
            return scope;
        }

        auto make_response_scope(
            const security_context& context,
            bool local_is_caller) -> protected_key_scope
        {
            protected_key_scope scope;
            scope.session_id = context.session_id;
            scope.caller_identity = local_is_caller ? context.local_identity : context.peer_identity;
            scope.destination_identity = local_is_caller ? context.peer_identity : context.local_identity;
            scope.direction = protected_rpc_direction::destination_to_caller;
            return scope;
        }

        auto prepare_envelope(
            const security_context& context,
            uint64_t counter) -> rpc::encrypted_payload
        {
            rpc::encrypted_payload envelope;
            envelope.session_id = context.session_id;
            envelope.session_epoch = context.session_epoch;
            envelope.e2e_counter = counter;
            return envelope;
        }

        auto validate_envelope_context(
            const rpc::encrypted_payload& envelope,
            const security_context& context) -> bool
        {
            return envelope.session_id == context.session_id && envelope.session_epoch == context.session_epoch
                   && envelope.e2e_counter != protected_rpc_invalid_counter;
        }

        auto validate_visible_send_fields(
            const rpc::send_params& outer,
            const rpc::send_params& inner) -> bool
        {
            return outer.caller_zone_id == inner.caller_zone_id && outer.remote_object_id == inner.remote_object_id
                   && outer.request_id == inner.request_id;
        }

        auto validate_visible_post_fields(
            const rpc::post_params& outer,
            const rpc::post_params& inner) -> bool
        {
            return outer.caller_zone_id == inner.caller_zone_id && outer.remote_object_id == inner.remote_object_id;
        }
    } // namespace

    auto encrypted_payload_interface_id(uint64_t protocol_version) -> rpc::interface_ordinal
    {
        return rpc::interface_ordinal(rpc::id<rpc::encrypted_payload>::get(protocol_version));
    }

    auto is_protected_rpc_envelope(
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        uint64_t protocol_version) -> bool
    {
        return method_id == protected_rpc_outer_method_id
               && interface_id == encrypted_payload_interface_id(protocol_version);
    }

    auto protect_send_request(
        attestation_service& service,
        const security_context& context,
        rpc::send_params params) -> protected_rpc_result<protected_send_request>
    {
        if (!context.established)
            return rejected<protected_send_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");

        auto scope = make_request_scope(context, true, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), "failed to derive protected request key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_send_request_plaintext(params, counter.counter, context);
        if (!plaintext)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "failed to encode protected request");

        auto outer = params;
        outer.encoding_type = rpc::encoding::yas_binary;
        outer.tag = protected_rpc_envelope_tag;
        outer.interface_id = encrypted_payload_interface_id(params.protocol_version);
        outer.method_id = rpc::method(protected_rpc_outer_method_id);

        auto envelope = prepare_envelope(context, counter.counter);
        auto aad = make_send_request_aad(outer, envelope, protected_rpc_kind::send);
        if (!aad)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "failed to encode protected request aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!gcm_encrypt(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected request");

        auto wire = serialise_envelope(envelope);
        if (!wire)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "failed to serialise protected request");
        outer.in_data = std::move(*wire);

        protected_send_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_send_request(
        attestation_service& service,
        const rpc::send_params& outer) -> protected_rpc_result<protected_send_request>
    {
        auto envelope = deserialise_envelope(outer.in_data);
        if (!envelope)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "protected request envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_send_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected request session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_send_request>(
                rpc::error::SECURITY_ERROR(), "protected request session metadata mismatch");

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), "failed to derive protected request key");

        auto aad = make_send_request_aad(outer, *envelope, protected_rpc_kind::send);
        if (!aad)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "failed to encode protected request aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = gcm_decrypt(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_send_request>(
                rpc::error::SECURITY_ERROR(), "protected request authentication failed");

        auto decoded = decode_request_plaintext(*plaintext, protected_rpc_kind::send);
        if (!decoded)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "protected request plaintext is malformed");
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter || !validate_visible_send_fields(outer, decoded->send))
        {
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), "protected request binding mismatch");
        }

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        protected_send_request value;
        value.params = std::move(decoded->send);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }

    auto protect_send_response(
        attestation_service& service,
        const security_context& context,
        const rpc::send_params& outer_request,
        uint64_t request_counter,
        rpc::send_result response) -> protected_rpc_result<rpc::send_result>
    {
        if (!context.established || request_counter == protected_rpc_invalid_counter)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "protected response context is invalid");

        auto scope = make_response_scope(context, false);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "failed to derive protected response key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_response_plaintext(response, request_counter, counter.counter, context);
        if (!plaintext)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to encode protected response");

        auto envelope = prepare_envelope(context, counter.counter);
        auto aad = make_response_aad(outer_request, envelope, request_counter);
        if (!aad)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to encode protected response aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!gcm_encrypt(*key, nonce, *plaintext, *aad, envelope))
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected response");

        auto wire = serialise_envelope(envelope);
        if (!wire)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to serialise protected response");

        rpc::send_result outer;
        outer.error_code = rpc::error::OK();
        outer.out_buf = std::move(*wire);
        return accepted(std::move(outer));
    }

    auto unprotect_send_response(
        attestation_service& service,
        const security_context& context,
        const rpc::send_params& outer_request,
        uint64_t request_counter,
        rpc::send_result outer_response) -> protected_rpc_result<rpc::send_result>
    {
        if (outer_response.error_code != rpc::error::OK())
            return accepted(std::move(outer_response));

        auto envelope = deserialise_envelope(outer_response.out_buf);
        if (!envelope)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "protected response envelope is malformed");
        if (!validate_envelope_context(*envelope, context))
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "protected response session metadata mismatch");

        auto scope = make_response_scope(context, true);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "failed to derive protected response key");

        auto aad = make_response_aad(outer_request, *envelope, request_counter);
        if (!aad)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to encode protected response aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = gcm_decrypt(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "protected response authentication failed");

        auto decoded = decode_response_plaintext(*plaintext);
        if (!decoded)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "protected response plaintext is malformed");
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->request_counter != request_counter || decoded->response_counter != envelope->e2e_counter)
        {
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "protected response binding mismatch");
        }

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), counter.reason);

        return accepted(std::move(decoded->response));
    }

    auto protect_post_request(
        attestation_service& service,
        const security_context& context,
        rpc::post_params params) -> protected_rpc_result<protected_post_request>
    {
        if (!context.established)
            return rejected<protected_post_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");

        auto scope = make_request_scope(context, true, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "failed to derive protected post key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_post_request_plaintext(params, counter.counter, context);
        if (!plaintext)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "failed to encode protected post");

        auto outer = params;
        outer.encoding_type = rpc::encoding::yas_binary;
        outer.tag = protected_rpc_envelope_tag;
        outer.interface_id = encrypted_payload_interface_id(params.protocol_version);
        outer.method_id = rpc::method(protected_rpc_outer_method_id);

        auto envelope = prepare_envelope(context, counter.counter);
        auto aad = make_post_request_aad(outer, envelope);
        if (!aad)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "failed to encode protected post aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!gcm_encrypt(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected post");

        auto wire = serialise_envelope(envelope);
        if (!wire)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "failed to serialise protected post");
        outer.in_data = std::move(*wire);

        protected_post_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_post_request(
        attestation_service& service,
        const rpc::post_params& outer) -> protected_rpc_result<protected_post_request>
    {
        auto envelope = deserialise_envelope(outer.in_data);
        if (!envelope)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "protected post envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_post_request>(rpc::error::ZONE_NOT_SUPPORTED(), "protected post session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_post_request>(
                rpc::error::SECURITY_ERROR(), "protected post session metadata mismatch");

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "failed to derive protected post key");

        auto aad = make_post_request_aad(outer, *envelope);
        if (!aad)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "failed to encode protected post aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = gcm_decrypt(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "protected post authentication failed");

        auto decoded = decode_request_plaintext(*plaintext, protected_rpc_kind::post);
        if (!decoded)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "protected post plaintext is malformed");
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter || !validate_visible_post_fields(outer, decoded->post))
        {
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "protected post binding mismatch");
        }

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        protected_post_request value;
        value.params = std::move(decoded->post);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }
} // namespace canopy::security::attestation
