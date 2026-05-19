/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/protected_rpc.h>

#include <attestation/protected_rpc_protocol.h>
#include <security/attestation/aead.h>

#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        inline constexpr size_t bytes_per_kib = 1024U;
        inline constexpr size_t kib_per_mib = 1024U;
        inline constexpr size_t max_protected_field_size_mib = 64U;
        inline constexpr size_t max_protected_field_size = max_protected_field_size_mib * kib_per_mib * bytes_per_kib;
        inline constexpr size_t max_public_back_channel_entries = 4096U;
        inline constexpr uint64_t add_ref_options_valid_mask
            = static_cast<uint64_t>(rpc::add_ref_options::build_destination_route)
              | static_cast<uint64_t>(rpc::add_ref_options::build_caller_route)
              | static_cast<uint64_t>(rpc::add_ref_options::optimistic);
        inline constexpr uint64_t release_options_valid_mask = static_cast<uint64_t>(rpc::release_options::optimistic);

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

        auto route_only_remote_object(const rpc::remote_object& object) -> rpc::remote_object
        {
            return rpc::remote_object(object.as_zone());
        }

        auto payload_encoding(const std::optional<rpc::typed_payload>& payload) -> rpc::encoding
        {
            return payload ? payload->get_encoding() : rpc::encoding::not_set;
        }

        auto make_typed_payload(
            uint64_t type_id,
            rpc::encoding encoding,
            std::vector<char> payload) -> rpc::typed_payload
        {
            return rpc::typed_payload{type_id, encoding, std::move(payload)};
        }

        template<typename T> auto serialise_canonical(const T& value) -> std::optional<std::vector<uint8_t>>
        {
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            try
            {
                return rpc::to_canonical_crypto<std::vector<uint8_t>>(value);
            }
            catch (...)
            {
                return std::nullopt;
            }
#else
            (void)value;
            return std::nullopt;
#endif
        }

        template<typename T> auto deserialise_canonical(const std::vector<uint8_t>& bytes) -> std::optional<T>
        {
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            T value;
            try
            {
                if (!rpc::from_canonical_crypto(rpc::byte_span(bytes), value).empty())
                    return std::nullopt;
            }
            catch (...)
            {
                return std::nullopt;
            }
            return value;
#else
            (void)bytes;
            return std::nullopt;
#endif
        }

        // Common protected-record context used at the start of every canonical
        // AAD/plaintext struct. protocol_version is carried here because it is
        // shared schema context, not a marshaller-specific field.
        auto make_envelope_metadata(
            uint64_t protocol_version,
            std::string session_id,
            uint64_t session_epoch,
            uint64_t e2e_counter) -> rpc::attestation::envelope_metadata
        {
            rpc::attestation::envelope_metadata metadata;
            metadata.protocol_version = protocol_version;
            metadata.session_id = std::move(session_id);
            metadata.session_epoch = session_epoch;
            metadata.e2e_counter = e2e_counter;
            return metadata;
        }

        auto make_envelope_metadata(
            uint64_t protocol_version,
            const rpc::encrypted_payload& envelope) -> rpc::attestation::envelope_metadata
        {
            return make_envelope_metadata(
                protocol_version, envelope.session_id, envelope.session_epoch, envelope.e2e_counter);
        }

        auto make_envelope_metadata(
            uint64_t protocol_version,
            const security_context& context,
            uint64_t e2e_counter) -> rpc::attestation::envelope_metadata
        {
            return make_envelope_metadata(protocol_version, context.session_id, context.session_epoch, e2e_counter);
        }

        auto back_channels_are_reasonable(const std::vector<rpc::back_channel_entry>& entries) -> bool
        {
            if (entries.size() > max_public_back_channel_entries)
                return false;
            for (const auto& entry : entries)
            {
                if (entry.payload.size() > max_protected_field_size)
                    return false;
            }
            return true;
        }

        // AAD is AES-GCM "additional authenticated data": public routing context
        // authenticated by the tag but not encrypted. These helpers deliberately
        // do not include encrypted_payload.payload; OpenSSL authenticates the
        // ciphertext and tag through GCM itself.
        auto make_send_public_aad(
            const rpc::send_params& outer,
            const rpc::encrypted_payload& envelope) -> rpc::attestation::send_public_aad
        {
            rpc::attestation::send_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.caller_zone_id = outer.caller_zone_id;
            aad.destination_zone_id = outer.remote_object_id.as_zone();
            aad.interface_id = outer.interface_id;
            aad.method_id = outer.method_id;
            return aad;
        }

        // Builds the complete AAD byte string passed to AES-GCM for send-like
        // requests and protected send responses.
        auto make_send_request_aad(
            const rpc::send_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            return serialise_canonical(make_send_public_aad(outer, envelope));
        }

        // Builds the complete AAD byte string passed to AES-GCM for post requests.
        auto make_post_request_aad(
            const rpc::post_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::post_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.caller_zone_id = outer.caller_zone_id;
            aad.destination_zone_id = outer.remote_object_id.as_zone();
            aad.interface_id = outer.interface_id;
            aad.method_id = outer.method_id;
            return serialise_canonical(aad);
        }

        // Builds the complete AAD byte string passed to AES-GCM for add_ref requests.
        auto make_add_ref_request_aad(
            const rpc::add_ref_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::add_ref_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.destination_zone_id = outer.remote_object_id.as_zone();
            aad.caller_zone_id = outer.caller_zone_id;
            aad.requesting_zone_id = outer.requesting_zone_id;
            aad.add_ref_options = outer.build_out_param_channel;
            return serialise_canonical(aad);
        }

        // Builds the complete AAD byte string passed to AES-GCM for release requests.
        auto make_release_request_aad(
            const rpc::release_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::release_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.destination_zone_id = outer.remote_object_id.as_zone();
            aad.caller_zone_id = outer.caller_zone_id;
            aad.release_options = outer.options;
            return serialise_canonical(aad);
        }

        // Builds the complete AAD byte string passed to AES-GCM for try_cast requests.
        auto make_try_cast_request_aad(
            const rpc::try_cast_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::try_cast_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.caller_zone_id = outer.caller_zone_id;
            aad.destination_zone_id = outer.remote_object_id.as_zone();
            aad.interface_id = outer.interface_id;
            return serialise_canonical(aad);
        }

        // Builds the complete AAD byte string passed to AES-GCM for object_released requests.
        auto make_object_released_request_aad(
            const rpc::object_released_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::object_released_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.destination_zone_id = outer.remote_object_id.as_zone();
            aad.caller_zone_id = outer.caller_zone_id;
            return serialise_canonical(aad);
        }

        // Builds the complete AAD byte string passed to AES-GCM for protected transport_down requests.
        auto make_transport_down_request_aad(
            const rpc::transport_down_params& outer,
            const rpc::encrypted_payload& envelope) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::transport_down_public_aad aad;
            aad.envelope = make_envelope_metadata(outer.protocol_version, envelope);
            aad.destination_zone_id = outer.destination_zone_id;
            aad.caller_zone_id = outer.caller_zone_id;
            return serialise_canonical(aad);
        }

        // Response AAD is public authenticated context for the encrypted response.
        // The accepted attestation request counter is enough to bind the response
        // to one protected send within this session/key scope; caller-owned route
        // fields from the original request do not need to be repeated here.
        auto make_response_aad(
            uint64_t protocol_version,
            const rpc::encrypted_payload& envelope,
            uint64_t request_counter) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::response_aad aad;
            aad.envelope = make_envelope_metadata(protocol_version, envelope);
            aad.request_counter = request_counter;
            return serialise_canonical(aad);
        }

        auto make_send_plaintext(
            const rpc::send_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> rpc::attestation::send_plaintext
        {
            rpc::attestation::send_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.remote_object_id = params.remote_object_id;
            plaintext.interface_id = params.interface_id;
            plaintext.method_id = params.method_id;
            plaintext.in_data = params.in_data;
            plaintext.in_back_channel = params.in_back_channel;
            return plaintext;
        }

        auto make_post_plaintext(
            const rpc::post_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> rpc::attestation::post_plaintext
        {
            rpc::attestation::post_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.remote_object_id = params.remote_object_id;
            plaintext.interface_id = params.interface_id;
            plaintext.method_id = params.method_id;
            plaintext.in_data = params.in_data;
            plaintext.in_back_channel = params.in_back_channel;
            return plaintext;
        }

        auto encode_send_request_plaintext(
            const rpc::send_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            return serialise_canonical(make_send_plaintext(params, e2e_counter, context));
        }

        auto encode_post_request_plaintext(
            const rpc::post_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            return serialise_canonical(make_post_plaintext(params, e2e_counter, context));
        }

        auto encode_add_ref_request_plaintext(
            const rpc::add_ref_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::add_ref_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.remote_object_id = params.remote_object_id;
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.requesting_zone_id = params.requesting_zone_id;
            plaintext.add_ref_options = params.build_out_param_channel;
            plaintext.in_back_channel = params.in_back_channel;
            plaintext.payload = params.payload;
            return serialise_canonical(plaintext);
        }

        auto encode_release_request_plaintext(
            const rpc::release_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::release_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.remote_object_id = params.remote_object_id;
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.release_options = params.options;
            plaintext.in_back_channel = params.in_back_channel;
            plaintext.payload = params.payload;
            return serialise_canonical(plaintext);
        }

        auto encode_try_cast_request_plaintext(
            const rpc::try_cast_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::try_cast_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.remote_object_id = params.remote_object_id;
            plaintext.interface_id = params.interface_id;
            plaintext.in_back_channel = params.in_back_channel;
            plaintext.payload = params.payload;
            return serialise_canonical(plaintext);
        }

        auto encode_object_released_request_plaintext(
            const rpc::object_released_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::object_released_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.remote_object_id = params.remote_object_id;
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.in_back_channel = params.in_back_channel;
            plaintext.payload = params.payload;
            return serialise_canonical(plaintext);
        }

        auto encode_transport_down_request_plaintext(
            const rpc::transport_down_params& params,
            uint64_t e2e_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::transport_down_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(params.protocol_version, context, e2e_counter);
            plaintext.destination_zone_id = params.destination_zone_id;
            plaintext.caller_zone_id = params.caller_zone_id;
            plaintext.in_back_channel = params.in_back_channel;
            plaintext.payload = params.payload;
            return serialise_canonical(plaintext);
        }

        struct decoded_request
        {
            protected_rpc_kind kind{protected_rpc_kind::send};
            uint64_t e2e_counter{0};
            std::string session_id;
            uint64_t session_epoch{0};
            rpc::send_params send;
            rpc::post_params post;
            rpc::add_ref_params add_ref;
            rpc::release_params release;
            rpc::try_cast_params try_cast;
            rpc::object_released_params object_released;
            rpc::transport_down_params transport_down;
        };

        auto decode_request_plaintext(
            const std::vector<uint8_t>& plaintext,
            protected_rpc_kind expected_kind) -> std::optional<decoded_request>
        {
            if (expected_kind == protected_rpc_kind::send)
            {
                auto decoded = deserialise_canonical<rpc::attestation::send_plaintext>(plaintext);
                if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                    return std::nullopt;

                decoded_request result;
                result.kind = expected_kind;
                result.e2e_counter = decoded->envelope.e2e_counter;
                result.session_id = decoded->envelope.session_id;
                result.session_epoch = decoded->envelope.session_epoch;
                result.send.protocol_version = decoded->envelope.protocol_version;
                result.send.tag = 0;
                result.send.caller_zone_id = decoded->caller_zone_id;
                result.send.remote_object_id = decoded->remote_object_id;
                result.send.interface_id = decoded->interface_id;
                result.send.method_id = decoded->method_id;
                result.send.in_data = std::move(decoded->in_data);
                result.send.in_back_channel = std::move(decoded->in_back_channel);
                return result;
            }

            auto decoded = deserialise_canonical<rpc::attestation::post_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                return std::nullopt;

            decoded_request result;
            result.kind = expected_kind;
            result.e2e_counter = decoded->envelope.e2e_counter;
            result.session_id = decoded->envelope.session_id;
            result.session_epoch = decoded->envelope.session_epoch;
            result.post.protocol_version = decoded->envelope.protocol_version;
            result.post.tag = 0;
            result.post.caller_zone_id = decoded->caller_zone_id;
            result.post.remote_object_id = decoded->remote_object_id;
            result.post.interface_id = decoded->interface_id;
            result.post.method_id = decoded->method_id;
            result.post.in_data = std::move(decoded->in_data);
            result.post.in_back_channel = std::move(decoded->in_back_channel);
            return result;
        }

        auto decode_add_ref_request_plaintext(const std::vector<uint8_t>& plaintext) -> std::optional<decoded_request>
        {
            auto decoded = deserialise_canonical<rpc::attestation::add_ref_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                return std::nullopt;

            if ((static_cast<uint64_t>(decoded->add_ref_options) & ~add_ref_options_valid_mask) != 0)
                return std::nullopt;

            decoded_request result;
            result.kind = protected_rpc_kind::add_ref;
            result.e2e_counter = decoded->envelope.e2e_counter;
            result.session_id = decoded->envelope.session_id;
            result.session_epoch = decoded->envelope.session_epoch;
            result.add_ref.protocol_version = decoded->envelope.protocol_version;
            result.add_ref.remote_object_id = decoded->remote_object_id;
            result.add_ref.caller_zone_id = decoded->caller_zone_id;
            result.add_ref.requesting_zone_id = decoded->requesting_zone_id;
            result.add_ref.build_out_param_channel = decoded->add_ref_options;
            result.add_ref.in_back_channel = std::move(decoded->in_back_channel);
            result.add_ref.payload = std::move(decoded->payload);
            return result;
        }

        auto decode_release_request_plaintext(const std::vector<uint8_t>& plaintext) -> std::optional<decoded_request>
        {
            auto decoded = deserialise_canonical<rpc::attestation::release_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                return std::nullopt;

            if ((static_cast<uint64_t>(decoded->release_options) & ~release_options_valid_mask) != 0)
                return std::nullopt;

            decoded_request result;
            result.kind = protected_rpc_kind::release;
            result.e2e_counter = decoded->envelope.e2e_counter;
            result.session_id = decoded->envelope.session_id;
            result.session_epoch = decoded->envelope.session_epoch;
            result.release.protocol_version = decoded->envelope.protocol_version;
            result.release.remote_object_id = decoded->remote_object_id;
            result.release.caller_zone_id = decoded->caller_zone_id;
            result.release.options = decoded->release_options;
            result.release.in_back_channel = std::move(decoded->in_back_channel);
            result.release.payload = std::move(decoded->payload);
            return result;
        }

        auto decode_try_cast_request_plaintext(const std::vector<uint8_t>& plaintext) -> std::optional<decoded_request>
        {
            auto decoded = deserialise_canonical<rpc::attestation::try_cast_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                return std::nullopt;

            decoded_request result;
            result.kind = protected_rpc_kind::try_cast;
            result.e2e_counter = decoded->envelope.e2e_counter;
            result.session_id = decoded->envelope.session_id;
            result.session_epoch = decoded->envelope.session_epoch;
            result.try_cast.protocol_version = decoded->envelope.protocol_version;
            result.try_cast.caller_zone_id = decoded->caller_zone_id;
            result.try_cast.remote_object_id = decoded->remote_object_id;
            result.try_cast.interface_id = decoded->interface_id;
            result.try_cast.in_back_channel = std::move(decoded->in_back_channel);
            result.try_cast.payload = std::move(decoded->payload);
            return result;
        }

        auto decode_object_released_request_plaintext(const std::vector<uint8_t>& plaintext)
            -> std::optional<decoded_request>
        {
            auto decoded = deserialise_canonical<rpc::attestation::object_released_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                return std::nullopt;

            decoded_request result;
            result.kind = protected_rpc_kind::object_released;
            result.e2e_counter = decoded->envelope.e2e_counter;
            result.session_id = decoded->envelope.session_id;
            result.session_epoch = decoded->envelope.session_epoch;
            result.object_released.protocol_version = decoded->envelope.protocol_version;
            result.object_released.remote_object_id = decoded->remote_object_id;
            result.object_released.caller_zone_id = decoded->caller_zone_id;
            result.object_released.in_back_channel = std::move(decoded->in_back_channel);
            result.object_released.payload = std::move(decoded->payload);
            return result;
        }

        auto decode_transport_down_request_plaintext(const std::vector<uint8_t>& plaintext)
            -> std::optional<decoded_request>
        {
            auto decoded = deserialise_canonical<rpc::attestation::transport_down_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->in_back_channel))
                return std::nullopt;

            decoded_request result;
            result.kind = protected_rpc_kind::transport_down;
            result.e2e_counter = decoded->envelope.e2e_counter;
            result.session_id = decoded->envelope.session_id;
            result.session_epoch = decoded->envelope.session_epoch;
            result.transport_down.protocol_version = decoded->envelope.protocol_version;
            result.transport_down.destination_zone_id = decoded->destination_zone_id;
            result.transport_down.caller_zone_id = decoded->caller_zone_id;
            result.transport_down.in_back_channel = std::move(decoded->in_back_channel);
            result.transport_down.payload = std::move(decoded->payload);
            return result;
        }

        auto encode_response_plaintext(
            const rpc::send_result& response,
            uint64_t protocol_version,
            uint64_t response_counter,
            const security_context& context) -> std::optional<std::vector<uint8_t>>
        {
            rpc::attestation::response_plaintext plaintext;
            plaintext.envelope = make_envelope_metadata(protocol_version, context, response_counter);
            plaintext.error_code = static_cast<int64_t>(response.error_code);
            plaintext.out_buf = response.out_buf;
            plaintext.out_back_channel = response.out_back_channel;
            return serialise_canonical(plaintext);
        }

        struct decoded_response
        {
            rpc::send_result response;
            uint64_t response_counter{0};
            std::string session_id;
            uint64_t session_epoch{0};
        };

        auto decode_response_plaintext(const std::vector<uint8_t>& plaintext) -> std::optional<decoded_response>
        {
            auto decoded = deserialise_canonical<rpc::attestation::response_plaintext>(plaintext);
            if (!decoded || !back_channels_are_reasonable(decoded->out_back_channel))
            {
                return std::nullopt;
            }
            if (decoded->error_code < std::numeric_limits<int>::min()
                || decoded->error_code > std::numeric_limits<int>::max())
            {
                return std::nullopt;
            }

            decoded_response result;
            result.response.error_code = static_cast<int>(decoded->error_code);
            result.response.out_buf = std::move(decoded->out_buf);
            result.response.out_back_channel = std::move(decoded->out_back_channel);
            result.response_counter = decoded->envelope.e2e_counter;
            result.session_id = std::move(decoded->envelope.session_id);
            result.session_epoch = decoded->envelope.session_epoch;
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

        // RPC-specific adapter around the AEAD helper. The OpenSSL/SGXSSL call
        // sequence lives in aead.cpp; protected_rpc.cpp only fills the wire
        // envelope fields after encryption succeeds.
        auto encrypt_into_envelope(
            const aead_key_material& key,
            const std::array<
                uint8_t,
                aead_nonce_size>& nonce,
            const std::vector<uint8_t>& plaintext,
            const std::vector<uint8_t>& aad,
            rpc::encrypted_payload& envelope) -> bool
        {
            auto ciphertext = encrypt_aes_256_gcm(key, nonce, plaintext, aad);
            if (!ciphertext)
                return false;
            envelope.payload = std::move(ciphertext->payload);
            envelope.authentication_tag = std::move(ciphertext->authentication_tag);
            return true;
        }

        // Returns plaintext only when AEAD verification succeeds for both the
        // ciphertext and the exact public AAD bytes computed by this layer.
        auto decrypt_envelope(
            const aead_key_material& key,
            const std::array<
                uint8_t,
                aead_nonce_size>& nonce,
            const rpc::encrypted_payload& envelope,
            const std::vector<uint8_t>& aad) -> std::optional<std::vector<uint8_t>>
        {
            return decrypt_aes_256_gcm(key, nonce, envelope.payload, envelope.authentication_tag, aad);
        }

        auto serialise_envelope(
            const rpc::encrypted_payload& envelope,
            rpc::encoding encoding) -> std::optional<std::vector<char>>
        {
            if (encoding == rpc::encoding::not_set)
                return std::nullopt;

            try
            {
                return rpc::serialise<std::vector<char>>(envelope, encoding);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        auto deserialise_envelope(
            const std::vector<char>& bytes,
            rpc::encoding encoding) -> std::optional<rpc::encrypted_payload>
        {
            if (encoding == rpc::encoding::not_set)
                return std::nullopt;

            rpc::encrypted_payload envelope;
            try
            {
                if (!rpc::deserialise(encoding, rpc::byte_span(bytes), envelope).empty())
                    return std::nullopt;
            }
            catch (...)
            {
                return std::nullopt;
            }
            if (envelope.session_id.empty() || envelope.session_epoch == protected_rpc_invalid_counter
                || envelope.e2e_counter == protected_rpc_invalid_counter || envelope.payload.size() > max_protected_field_size
                || envelope.authentication_tag.size() != aead_aes_256_gcm_tag_size)
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

        // Responses always use the destination-to-caller key direction.
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

        // Prepares the public encrypted_payload header; ciphertext and tag are
        // filled only after AEAD encryption succeeds.
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

        // Checks that the public envelope still points at the session context
        // used to derive the AEAD key.
        auto validate_envelope_context(
            const rpc::encrypted_payload& envelope,
            const security_context& context) -> bool
        {
            return envelope.session_id == context.session_id && envelope.session_epoch == context.session_epoch
                   && envelope.e2e_counter != protected_rpc_invalid_counter;
        }

        // Visible route fields are repeated inside the encrypted plaintext and
        // checked after decrypting to catch confused-carrier bugs.
        auto validate_visible_send_fields(
            const rpc::send_params& outer,
            const rpc::send_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version && outer.caller_zone_id == inner.caller_zone_id
                   && outer.remote_object_id.get_address().same_zone(inner.remote_object_id.get_address());
        }

        auto validate_visible_post_fields(
            const rpc::post_params& outer,
            const rpc::post_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version && outer.caller_zone_id == inner.caller_zone_id
                   && outer.remote_object_id.get_address().same_zone(inner.remote_object_id.get_address());
        }

        auto validate_visible_add_ref_fields(
            const rpc::add_ref_params& outer,
            const rpc::add_ref_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version
                   && outer.remote_object_id.get_address().same_zone(inner.remote_object_id.get_address())
                   && outer.caller_zone_id == inner.caller_zone_id && outer.requesting_zone_id == inner.requesting_zone_id
                   && outer.build_out_param_channel == inner.build_out_param_channel;
        }

        auto validate_visible_release_fields(
            const rpc::release_params& outer,
            const rpc::release_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version
                   && outer.remote_object_id.get_address().same_zone(inner.remote_object_id.get_address())
                   && outer.caller_zone_id == inner.caller_zone_id && outer.options == inner.options;
        }

        auto validate_visible_try_cast_fields(
            const rpc::try_cast_params& outer,
            const rpc::try_cast_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version && outer.caller_zone_id == inner.caller_zone_id
                   && outer.remote_object_id.get_address().same_zone(inner.remote_object_id.get_address());
        }

        auto validate_visible_object_released_fields(
            const rpc::object_released_params& outer,
            const rpc::object_released_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version
                   && outer.remote_object_id.get_address().same_zone(inner.remote_object_id.get_address())
                   && outer.caller_zone_id == inner.caller_zone_id;
        }

        auto validate_visible_transport_down_fields(
            const rpc::transport_down_params& outer,
            const rpc::transport_down_params& inner) -> bool
        {
            return outer.protocol_version == inner.protocol_version && outer.destination_zone_id == inner.destination_zone_id
                   && outer.caller_zone_id == inner.caller_zone_id;
        }
    } // namespace

    auto encrypted_payload_type_id(uint64_t protocol_version) -> uint64_t
    {
        return rpc::id<rpc::encrypted_payload>::get(protocol_version);
    }

    auto encrypted_payload_interface_id(uint64_t protocol_version) -> rpc::interface_ordinal
    {
        return rpc::interface_ordinal(encrypted_payload_type_id(protocol_version));
    }

    auto is_protected_rpc_payload(
        uint64_t payload_type_id,
        uint64_t protocol_version) -> bool
    {
        return payload_type_id == encrypted_payload_type_id(protocol_version);
    }

    auto is_protected_rpc_payload(
        const std::optional<rpc::typed_payload>& payload,
        uint64_t protocol_version) -> bool
    {
        return payload && is_protected_rpc_payload(payload->get_type_id(), protocol_version);
    }

    auto is_protected_rpc_envelope(
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        uint64_t protocol_version) -> bool
    {
        return method_id == protected_rpc_carrier_method_id
               && interface_id == encrypted_payload_interface_id(protocol_version);
    }

    auto protect_send_request(
        attestation_service& service,
        const security_context& context,
        rpc::send_params params) -> protected_rpc_result<protected_send_request>
    {
        // Protect the original send as canonical_crypto plaintext, then reuse
        // send_params as a routing-only carrier for the encrypted_payload blob.
        if (!context.established)
            return rejected<protected_send_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");

        // Caller -> destination traffic gets its own AEAD key scope and
        // monotonic end-to-end counter; the counter is also the nonce input.
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

        // Keep only route-visible fields outside the encrypted envelope. The
        // destination reconstructs the private object/interface/method/data
        // values from the plaintext after authentication succeeds.
        auto outer = params;
        outer.tag = 0;
        outer.remote_object_id = route_only_remote_object(params.remote_object_id);
        outer.interface_id = encrypted_payload_interface_id(params.protocol_version);
        outer.method_id = rpc::method(protected_rpc_carrier_method_id);

        // AAD binds the visible carrier fields to the ciphertext without
        // encrypting them. Any intermediate change to these fields invalidates
        // the GCM authentication tag.
        auto envelope = prepare_envelope(context, counter.counter);
        auto aad = make_send_request_aad(outer, envelope);
        if (!aad)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "failed to encode protected request aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected request");

        auto wire = serialise_envelope(envelope, outer.encoding_type);
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
        // The public carrier is parsed first, but none of its private meaning
        // is trusted until the AEAD tag and plaintext binding checks pass.
        auto envelope = deserialise_envelope(outer.in_data, outer.encoding_type);
        if (!envelope)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "protected request envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_send_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected request session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_send_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected request session metadata mismatch");

        // The receiver derives the same caller -> destination key using the
        // remote identity as caller and the local identity as destination.
        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_send_request>(rpc::error::SECURITY_ERROR(), "failed to derive protected request key");

        // Recompute AAD from the received public carrier so route-field
        // tampering fails before any plaintext is accepted.
        auto aad = make_send_request_aad(outer, *envelope);
        if (!aad)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "failed to encode protected request aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_send_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected request authentication failed");

        auto decoded = decode_request_plaintext(*plaintext, protected_rpc_kind::send);
        if (!decoded)
            return rejected<protected_send_request>(rpc::error::INVALID_DATA(), "protected request plaintext is malformed");
        // The plaintext repeats route-critical fields so a valid envelope
        // cannot be replayed through a different visible carrier.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter || !validate_visible_send_fields(outer, decoded->send))
        {
            return rejected<protected_send_request>(rpc::error::FRAUDULANT_REQUEST(), "protected request binding mismatch");
        }
        decoded->send.request_id = outer.request_id;
        decoded->send.encoding_type = outer.encoding_type;
        decoded->send.in_back_channel = outer.in_back_channel;

        // Replay protection is committed only after authentication and binding
        // validation have both succeeded.
        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_send_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

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
        // Send responses use the destination -> caller key direction and bind
        // the response to the accepted request counter.
        if (!context.established || request_counter == protected_rpc_invalid_counter)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "protected response context is invalid");

        auto scope = make_response_scope(context, false);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "failed to derive protected response key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_response_plaintext(response, outer_request.protocol_version, counter.counter, context);
        if (!plaintext)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to encode protected response");

        // Only RPC carrier success is visible here. The application error code
        // is part of the encrypted response plaintext.
        auto envelope = prepare_envelope(context, counter.counter);
        auto aad = make_response_aad(outer_request.protocol_version, envelope, request_counter);
        if (!aad)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to encode protected response aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected response");

        auto wire = serialise_envelope(envelope, outer_request.encoding_type);
        if (!wire)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to serialise protected response");

        rpc::send_result outer;
        outer.error_code = rpc::error::OK();
        outer.out_buf = std::move(*wire);
        outer.out_back_channel = std::move(response.out_back_channel);
        return accepted(std::move(outer));
    }

    auto unprotect_send_response(
        attestation_service& service,
        const security_context& context,
        const rpc::send_params& outer_request,
        uint64_t request_counter,
        rpc::send_result outer_response) -> protected_rpc_result<rpc::send_result>
    {
        // Public carrier failures are allowed only for standard RPC control
        // statuses. Application result codes must stay inside the envelope.
        if (outer_response.error_code != rpc::error::OK())
        {
            if (!rpc::error::is_public_control_status(outer_response.error_code))
            {
                return rejected<rpc::send_result>(
                    rpc::error::PROTOCOL_ERROR(), "protected response exposed a non-RPC carrier status");
            }
            return accepted(std::move(outer_response));
        }

        auto envelope = deserialise_envelope(outer_response.out_buf, outer_request.encoding_type);
        if (!envelope)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "protected response envelope is malformed");
        if (!validate_envelope_context(*envelope, context))
            return rejected<rpc::send_result>(
                rpc::error::FRAUDULANT_REQUEST(), "protected response session metadata mismatch");

        auto scope = make_response_scope(context, true);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<rpc::send_result>(rpc::error::SECURITY_ERROR(), "failed to derive protected response key");

        // The request counter in AAD prevents a valid response from being
        // detached from the protected send that caused it.
        auto aad = make_response_aad(outer_request.protocol_version, *envelope, request_counter);
        if (!aad)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "failed to encode protected response aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<rpc::send_result>(rpc::error::FRAUDULANT_REQUEST(), "protected response authentication failed");

        auto decoded = decode_response_plaintext(*plaintext);
        if (!decoded)
            return rejected<rpc::send_result>(rpc::error::INVALID_DATA(), "protected response plaintext is malformed");
        // Session metadata is repeated inside the plaintext to catch malformed
        // or confused carriers even after the AEAD tag validates.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->response_counter != envelope->e2e_counter)
        {
            return rejected<rpc::send_result>(rpc::error::FRAUDULANT_REQUEST(), "protected response binding mismatch");
        }

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<rpc::send_result>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        decoded->response.out_back_channel = std::move(outer_response.out_back_channel);
        return accepted(std::move(decoded->response));
    }

    auto protect_post_request(
        attestation_service& service,
        const security_context& context,
        rpc::post_params params) -> protected_rpc_result<protected_post_request>
    {
        // Post follows the send request shape but has no response path; the
        // original payload and private dispatch fields are encrypted together.
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

        // Intermediates see only routing information and the protected payload
        // carrier identity.
        auto outer = params;
        outer.tag = 0;
        outer.remote_object_id = route_only_remote_object(params.remote_object_id);
        outer.interface_id = encrypted_payload_interface_id(params.protocol_version);
        outer.method_id = rpc::method(protected_rpc_carrier_method_id);

        auto envelope = prepare_envelope(context, counter.counter);
        auto aad = make_post_request_aad(outer, envelope);
        if (!aad)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "failed to encode protected post aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected post");

        auto wire = serialise_envelope(envelope, outer.encoding_type);
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
        // Post request receive-side validation mirrors send request validation,
        // except there is no request_id/response correlation to preserve.
        auto envelope = deserialise_envelope(outer.in_data, outer.encoding_type);
        if (!envelope)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "protected post envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_post_request>(rpc::error::ZONE_NOT_SUPPORTED(), "protected post session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_post_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected post session metadata mismatch");

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_post_request>(rpc::error::SECURITY_ERROR(), "failed to derive protected post key");

        auto aad = make_post_request_aad(outer, *envelope);
        if (!aad)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "failed to encode protected post aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_post_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected post authentication failed");

        auto decoded = decode_request_plaintext(*plaintext, protected_rpc_kind::post);
        if (!decoded)
            return rejected<protected_post_request>(rpc::error::INVALID_DATA(), "protected post plaintext is malformed");
        // The route-visible caller and destination zone must match the private
        // plaintext view before the post is delivered to the stub.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter || !validate_visible_post_fields(outer, decoded->post))
        {
            return rejected<protected_post_request>(rpc::error::FRAUDULANT_REQUEST(), "protected post binding mismatch");
        }
        decoded->post.encoding_type = outer.encoding_type;
        decoded->post.in_back_channel = outer.in_back_channel;

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_post_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        protected_post_request value;
        value.params = std::move(decoded->post);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }

    auto protect_add_ref_request(
        attestation_service& service,
        const security_context& context,
        rpc::add_ref_params params,
        rpc::encoding envelope_encoding) -> protected_rpc_result<protected_add_ref_request>
    {
        // add_ref must remain routable before the callee has a usable proxy, so
        // only the zone-level address fields stay public. Object id and payload
        // details are recovered from the encrypted plaintext.
        if (!context.established)
            return rejected<protected_add_ref_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");
        if (is_protected_rpc_payload(params.payload, params.protocol_version))
            return rejected<protected_add_ref_request>(rpc::error::INVALID_DATA(), "add_ref payload is already protected");
        if (envelope_encoding == rpc::encoding::not_set)
            envelope_encoding = payload_encoding(params.payload);
        if (envelope_encoding == rpc::encoding::not_set)
            return rejected<protected_add_ref_request>(
                rpc::error::INVALID_DATA(), "protected add_ref requires an envelope encoding");

        auto scope = make_request_scope(context, true, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_add_ref_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected add_ref key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_add_ref_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_add_ref_request_plaintext(params, counter.counter, context);
        if (!plaintext)
            return rejected<protected_add_ref_request>(rpc::error::INVALID_DATA(), "failed to encode protected add_ref");

        // The typed payload slot becomes the encrypted_payload carrier. Its
        // encoding is explicitly supplied by the service/transport call path.
        auto outer = params;
        outer.remote_object_id = route_only_remote_object(params.remote_object_id);

        auto envelope = prepare_envelope(context, counter.counter);
        outer.payload = make_typed_payload(encrypted_payload_type_id(params.protocol_version), envelope_encoding, {});
        auto aad = make_add_ref_request_aad(outer, envelope);
        if (!aad)
            return rejected<protected_add_ref_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected add_ref aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_add_ref_request>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected add_ref");

        auto wire = serialise_envelope(envelope, envelope_encoding);
        if (!wire)
            return rejected<protected_add_ref_request>(rpc::error::INVALID_DATA(), "failed to serialise protected add_ref");
        outer.payload->set_payload(std::move(*wire));

        protected_add_ref_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_add_ref_request(
        attestation_service& service,
        const rpc::add_ref_params& outer) -> protected_rpc_result<protected_add_ref_request>
    {
        // Protected add_ref requires the typed payload carrier; an absent or
        // differently typed payload is handled by the caller's policy path.
        if (!is_protected_rpc_payload(outer.payload, outer.protocol_version))
            return rejected<protected_add_ref_request>(rpc::error::INVALID_DATA(), "add_ref payload is not protected");

        auto envelope = deserialise_envelope(outer.payload->get_payload(), outer.payload->get_encoding());
        if (!envelope)
            return rejected<protected_add_ref_request>(
                rpc::error::INVALID_DATA(), "protected add_ref envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_add_ref_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected add_ref session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_add_ref_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected add_ref session metadata mismatch");

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_add_ref_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected add_ref key");

        auto aad = make_add_ref_request_aad(outer, *envelope);
        if (!aad)
            return rejected<protected_add_ref_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected add_ref aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_add_ref_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected add_ref authentication failed");

        auto decoded = decode_add_ref_request_plaintext(*plaintext);
        if (!decoded)
            return rejected<protected_add_ref_request>(
                rpc::error::INVALID_DATA(), "protected add_ref plaintext is malformed");
        // The public routing hint must still describe the same destination zone
        // and reference-count operation that the source signed/encrypted.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter || !validate_visible_add_ref_fields(outer, decoded->add_ref))
        {
            return rejected<protected_add_ref_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected add_ref binding mismatch");
        }
        decoded->add_ref.request_id = outer.request_id;
        decoded->add_ref.in_back_channel = outer.in_back_channel;

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_add_ref_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        protected_add_ref_request value;
        value.params = std::move(decoded->add_ref);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }

    auto protect_release_request(
        attestation_service& service,
        const security_context& context,
        rpc::release_params params,
        rpc::encoding envelope_encoding) -> protected_rpc_result<protected_release_request>
    {
        // release protects the private object id and any typed payload while
        // leaving enough route context for intermediate transports.
        if (!context.established)
            return rejected<protected_release_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");
        if (is_protected_rpc_payload(params.payload, params.protocol_version))
            return rejected<protected_release_request>(rpc::error::INVALID_DATA(), "release payload is already protected");
        if (envelope_encoding == rpc::encoding::not_set)
            envelope_encoding = payload_encoding(params.payload);
        if (envelope_encoding == rpc::encoding::not_set)
            return rejected<protected_release_request>(
                rpc::error::INVALID_DATA(), "protected release requires an envelope encoding");

        auto scope = make_request_scope(context, true, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_release_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected release key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_release_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_release_request_plaintext(params, counter.counter, context);
        if (!plaintext)
            return rejected<protected_release_request>(rpc::error::INVALID_DATA(), "failed to encode protected release");

        // Keep the destination zone public, but hide the destination object id
        // inside the encrypted plaintext.
        auto outer = params;
        outer.remote_object_id = route_only_remote_object(params.remote_object_id);

        auto envelope = prepare_envelope(context, counter.counter);
        outer.payload = make_typed_payload(encrypted_payload_type_id(params.protocol_version), envelope_encoding, {});
        auto aad = make_release_request_aad(outer, envelope);
        if (!aad)
            return rejected<protected_release_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected release aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_release_request>(rpc::error::SECURITY_ERROR(), "failed to encrypt protected release");

        auto wire = serialise_envelope(envelope, envelope_encoding);
        if (!wire)
            return rejected<protected_release_request>(rpc::error::INVALID_DATA(), "failed to serialise protected release");
        outer.payload->set_payload(std::move(*wire));

        protected_release_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_release_request(
        attestation_service& service,
        const rpc::release_params& outer) -> protected_rpc_result<protected_release_request>
    {
        // A release without an authenticated add_ref should be rejected by
        // higher-level reference accounting; this layer verifies the envelope
        // and preserves the route-visible fields.
        if (!is_protected_rpc_payload(outer.payload, outer.protocol_version))
            return rejected<protected_release_request>(rpc::error::INVALID_DATA(), "release payload is not protected");

        auto envelope = deserialise_envelope(outer.payload->get_payload(), outer.payload->get_encoding());
        if (!envelope)
            return rejected<protected_release_request>(
                rpc::error::INVALID_DATA(), "protected release envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_release_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected release session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_release_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected release session metadata mismatch");

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_release_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected release key");

        auto aad = make_release_request_aad(outer, *envelope);
        if (!aad)
            return rejected<protected_release_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected release aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_release_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected release authentication failed");

        auto decoded = decode_release_request_plaintext(*plaintext);
        if (!decoded)
            return rejected<protected_release_request>(
                rpc::error::INVALID_DATA(), "protected release plaintext is malformed");
        // The caller, destination zone, and release flags are public because
        // transports need them; the full object identity remains private.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter || !validate_visible_release_fields(outer, decoded->release))
        {
            return rejected<protected_release_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected release binding mismatch");
        }
        decoded->release.in_back_channel = outer.in_back_channel;

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_release_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        protected_release_request value;
        value.params = std::move(decoded->release);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }

    auto protect_try_cast_request(
        attestation_service& service,
        const security_context& context,
        rpc::try_cast_params params,
        rpc::encoding envelope_encoding) -> protected_rpc_result<protected_try_cast_request>
    {
        // try_cast is runtime metadata, but interface discovery is still private
        // to the two attested endpoints. The public interface_id is only the
        // encrypted_payload carrier type.
        if (!context.established)
            return rejected<protected_try_cast_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");
        if (is_protected_rpc_payload(params.payload, params.protocol_version))
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "try_cast payload is already protected");
        if (envelope_encoding == rpc::encoding::not_set)
            envelope_encoding = payload_encoding(params.payload);
        if (envelope_encoding == rpc::encoding::not_set)
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "protected try_cast requires an envelope encoding");

        auto scope = make_request_scope(context, true, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_try_cast_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected try_cast key");

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_try_cast_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_try_cast_request_plaintext(params, counter.counter, context);
        if (!plaintext)
            return rejected<protected_try_cast_request>(rpc::error::INVALID_DATA(), "failed to encode protected try_cast");

        // Preserve routeability while hiding the requested interface fingerprint
        // in the encrypted payload.
        auto outer = params;
        outer.remote_object_id = route_only_remote_object(params.remote_object_id);
        outer.interface_id = encrypted_payload_interface_id(params.protocol_version);

        auto envelope = prepare_envelope(context, counter.counter);
        outer.payload = make_typed_payload(encrypted_payload_type_id(params.protocol_version), envelope_encoding, {});
        auto aad = make_try_cast_request_aad(outer, envelope);
        if (!aad)
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected try_cast aad");

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
            return rejected<protected_try_cast_request>(
                rpc::error::SECURITY_ERROR(), "failed to encrypt protected try_cast");

        auto wire = serialise_envelope(envelope, envelope_encoding);
        if (!wire)
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "failed to serialise protected try_cast");
        outer.payload->set_payload(std::move(*wire));

        protected_try_cast_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_try_cast_request(
        attestation_service& service,
        const rpc::try_cast_params& outer) -> protected_rpc_result<protected_try_cast_request>
    {
        // The visible interface_id must be the carrier type, never the private
        // interface being queried.
        if (!is_protected_rpc_payload(outer.payload, outer.protocol_version))
            return rejected<protected_try_cast_request>(rpc::error::INVALID_DATA(), "try_cast payload is not protected");
        if (outer.interface_id != encrypted_payload_interface_id(outer.protocol_version))
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "try_cast interface id is not a protected payload carrier");

        auto envelope = deserialise_envelope(outer.payload->get_payload(), outer.payload->get_encoding());
        if (!envelope)
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "protected try_cast envelope is malformed");

        auto context = service.find_session(envelope->session_id);
        if (!context)
            return rejected<protected_try_cast_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected try_cast session is unknown");
        if (!validate_envelope_context(*envelope, *context))
            return rejected<protected_try_cast_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected try_cast session metadata mismatch");

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
            return rejected<protected_try_cast_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected try_cast key");

        auto aad = make_try_cast_request_aad(outer, *envelope);
        if (!aad)
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected try_cast aad");

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
            return rejected<protected_try_cast_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected try_cast authentication failed");

        auto decoded = decode_try_cast_request_plaintext(*plaintext);
        if (!decoded)
            return rejected<protected_try_cast_request>(
                rpc::error::INVALID_DATA(), "protected try_cast plaintext is malformed");
        // Caller and destination zone remain public for routing; the actual
        // interface fingerprint is accepted only from authenticated plaintext.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter
            || !validate_visible_try_cast_fields(outer, decoded->try_cast))
        {
            return rejected<protected_try_cast_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected try_cast binding mismatch");
        }
        decoded->try_cast.in_back_channel = outer.in_back_channel;

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_try_cast_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        protected_try_cast_request value;
        value.params = std::move(decoded->try_cast);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }

    auto protect_object_released_request(
        attestation_service& service,
        const security_context& context,
        rpc::object_released_params params,
        rpc::encoding envelope_encoding) -> protected_rpc_result<protected_object_released_request>
    {
        // object_released is generated by the attested destination side, so it
        // travels in the destination -> caller direction.
        if (!context.established)
            return rejected<protected_object_released_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");
        if (is_protected_rpc_payload(params.payload, params.protocol_version))
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "object_released payload is already protected");
        }
        if (envelope_encoding == rpc::encoding::not_set)
            envelope_encoding = payload_encoding(params.payload);
        if (envelope_encoding == rpc::encoding::not_set)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "protected object_released requires an envelope encoding");
        }

        auto scope = make_response_scope(context, false);
        auto key = service.derive_aead_key(scope);
        if (!key)
        {
            return rejected<protected_object_released_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected object_released key");
        }

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_object_released_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_object_released_request_plaintext(params, counter.counter, context);
        if (!plaintext)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected object_released");
        }

        // Intermediates need the destination zone but not the object id that was
        // released.
        auto outer = params;
        outer.remote_object_id = route_only_remote_object(params.remote_object_id);

        auto envelope = prepare_envelope(context, counter.counter);
        outer.payload = make_typed_payload(encrypted_payload_type_id(params.protocol_version), envelope_encoding, {});
        auto aad = make_object_released_request_aad(outer, envelope);
        if (!aad)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected object_released aad");
        }

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
        {
            return rejected<protected_object_released_request>(
                rpc::error::SECURITY_ERROR(), "failed to encrypt protected object_released");
        }

        auto wire = serialise_envelope(envelope, envelope_encoding);
        if (!wire)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "failed to serialise protected object_released");
        }
        outer.payload->set_payload(std::move(*wire));

        protected_object_released_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_object_released_request(
        attestation_service& service,
        const rpc::object_released_params& outer) -> protected_rpc_result<protected_object_released_request>
    {
        // This validates the zone-originated protected object_released path. A
        // route-layer status generated by an intermediate is handled elsewhere.
        if (!is_protected_rpc_payload(outer.payload, outer.protocol_version))
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "object_released payload is not protected");
        }

        auto envelope = deserialise_envelope(outer.payload->get_payload(), outer.payload->get_encoding());
        if (!envelope)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "protected object_released envelope is malformed");
        }

        auto context = service.find_session(envelope->session_id);
        if (!context)
        {
            return rejected<protected_object_released_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected object_released session is unknown");
        }
        if (!validate_envelope_context(*envelope, *context))
        {
            return rejected<protected_object_released_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected object_released session metadata mismatch");
        }

        auto scope = make_response_scope(*context, true);
        auto key = service.derive_aead_key(scope);
        if (!key)
        {
            return rejected<protected_object_released_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected object_released key");
        }

        auto aad = make_object_released_request_aad(outer, *envelope);
        if (!aad)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected object_released aad");
        }

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
        {
            return rejected<protected_object_released_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected object_released authentication failed");
        }

        auto decoded = decode_object_released_request_plaintext(*plaintext);
        if (!decoded)
        {
            return rejected<protected_object_released_request>(
                rpc::error::INVALID_DATA(), "protected object_released plaintext is malformed");
        }
        // The visible carrier may expose only the zone route. The full object id
        // is restored from the encrypted plaintext after authentication.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter
            || !validate_visible_object_released_fields(outer, decoded->object_released))
        {
            return rejected<protected_object_released_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected object_released binding mismatch");
        }
        decoded->object_released.in_back_channel = outer.in_back_channel;

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_object_released_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        protected_object_released_request value;
        value.params = std::move(decoded->object_released);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }

    auto protect_transport_down_request(
        attestation_service& service,
        const security_context& context,
        rpc::transport_down_params params,
        rpc::encoding envelope_encoding) -> protected_rpc_result<protected_transport_down_request>
    {
        // This is the protected zone-originated transport_down form. An
        // intermediate may still synthesize an unprotected route-layer failure
        // when it is the only source of liveness information.
        if (!context.established)
            return rejected<protected_transport_down_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "attestation session is not established");
        if (is_protected_rpc_payload(params.payload, params.protocol_version))
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "transport_down payload is already protected");
        }
        if (envelope_encoding == rpc::encoding::not_set)
            envelope_encoding = payload_encoding(params.payload);
        if (envelope_encoding == rpc::encoding::not_set)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "protected transport_down requires an envelope encoding");
        }

        auto scope = make_request_scope(context, true, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected transport_down key");
        }

        auto counter = service.next_send_counter(scope);
        if (!counter.accepted)
            return rejected<protected_transport_down_request>(rpc::error::SECURITY_ERROR(), counter.reason);

        auto plaintext = encode_transport_down_request_plaintext(params, counter.counter, context);
        if (!plaintext)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected transport_down");
        }

        // destination_zone_id and caller_zone_id remain public because the
        // shutdown notice has to be routed even when intermediates are untrusted.
        auto outer = params;

        auto envelope = prepare_envelope(context, counter.counter);
        outer.payload = make_typed_payload(encrypted_payload_type_id(params.protocol_version), envelope_encoding, {});
        auto aad = make_transport_down_request_aad(outer, envelope);
        if (!aad)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected transport_down aad");
        }

        auto nonce = make_nonce(service, *key, counter.counter);
        if (!encrypt_into_envelope(*key, nonce, *plaintext, *aad, envelope))
        {
            return rejected<protected_transport_down_request>(
                rpc::error::SECURITY_ERROR(), "failed to encrypt protected transport_down");
        }

        auto wire = serialise_envelope(envelope, envelope_encoding);
        if (!wire)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "failed to serialise protected transport_down");
        }
        outer.payload->set_payload(std::move(*wire));

        protected_transport_down_request value;
        value.params = std::move(outer);
        value.context = context;
        value.request_counter = counter.counter;
        return accepted(std::move(value));
    }

    auto unprotect_transport_down_request(
        attestation_service& service,
        const rpc::transport_down_params& outer) -> protected_rpc_result<protected_transport_down_request>
    {
        // Only a typed encrypted_payload is accepted on the protected path. The
        // policy layer decides whether an unprotected intermediate notice is
        // acceptable for a given route.
        if (!is_protected_rpc_payload(outer.payload, outer.protocol_version))
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "transport_down payload is not protected");
        }

        auto envelope = deserialise_envelope(outer.payload->get_payload(), outer.payload->get_encoding());
        if (!envelope)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "protected transport_down envelope is malformed");
        }

        auto context = service.find_session(envelope->session_id);
        if (!context)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::ZONE_NOT_SUPPORTED(), "protected transport_down session is unknown");
        }
        if (!validate_envelope_context(*envelope, *context))
        {
            return rejected<protected_transport_down_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected transport_down session metadata mismatch");
        }

        auto scope = make_request_scope(*context, false, protected_rpc_direction::caller_to_destination);
        auto key = service.derive_aead_key(scope);
        if (!key)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::SECURITY_ERROR(), "failed to derive protected transport_down key");
        }

        auto aad = make_transport_down_request_aad(outer, *envelope);
        if (!aad)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "failed to encode protected transport_down aad");
        }

        auto nonce = make_nonce(service, *key, envelope->e2e_counter);
        auto plaintext = decrypt_envelope(*key, nonce, *envelope, *aad);
        if (!plaintext)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected transport_down authentication failed");
        }

        auto decoded = decode_transport_down_request_plaintext(*plaintext);
        if (!decoded)
        {
            return rejected<protected_transport_down_request>(
                rpc::error::INVALID_DATA(), "protected transport_down plaintext is malformed");
        }
        // The public route endpoints must match the authenticated plaintext
        // before this is treated as a zone-originated transport_down.
        if (decoded->session_id != envelope->session_id || decoded->session_epoch != envelope->session_epoch
            || decoded->e2e_counter != envelope->e2e_counter
            || !validate_visible_transport_down_fields(outer, decoded->transport_down))
        {
            return rejected<protected_transport_down_request>(
                rpc::error::FRAUDULANT_REQUEST(), "protected transport_down binding mismatch");
        }
        decoded->transport_down.in_back_channel = outer.in_back_channel;

        auto counter = service.accept_receive_counter(scope, envelope->e2e_counter);
        if (!counter.accepted)
            return rejected<protected_transport_down_request>(rpc::error::FRAUDULANT_REQUEST(), counter.reason);

        protected_transport_down_request value;
        value.params = std::move(decoded->transport_down);
        value.context = std::move(*context);
        value.request_counter = envelope->e2e_counter;
        return accepted(std::move(value));
    }
} // namespace canopy::security::attestation
