/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/fake/fake_backend.h>

#include <limits>
#include <optional>
#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        struct fake_evidence
        {
            std::string backend_id;
            std::string security_domain_id;
            std::string zone_id;
            uint64_t transcript_id{0};
            std::vector<uint8_t> nonce;
            uint64_t signature{0};
        };

        constexpr size_t max_fake_evidence_field_size = size_t{1024U} * 1024U;

        auto can_append(
            const std::vector<uint8_t>& out,
            size_t size) -> bool
        {
            return size <= out.max_size() && out.size() <= out.max_size() - size;
        }

        auto append_u32(
            std::vector<uint8_t>& out,
            uint32_t value) -> bool
        {
            if (!can_append(out, sizeof(value)))
                return false;
            for (unsigned shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            return true;
        }

        auto append_u64(
            std::vector<uint8_t>& out,
            uint64_t value) -> bool
        {
            if (!can_append(out, sizeof(value)))
                return false;
            for (unsigned shift = 0; shift < 64; shift += 8)
                out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            return true;
        }

        auto append_bytes(
            std::vector<uint8_t>& out,
            const uint8_t* data,
            size_t size) -> bool
        {
            if (size > max_fake_evidence_field_size || size > std::numeric_limits<uint32_t>::max())
                return false;
            if (size > 0 && data == nullptr)
                return false;
            if (!can_append(out, sizeof(uint32_t) + size))
                return false;

            if (!append_u32(out, static_cast<uint32_t>(size)))
                return false;
            if (size != 0)
                out.insert(out.end(), data, data + size);
            return true;
        }

        auto append_bytes(
            std::vector<uint8_t>& out,
            const std::vector<uint8_t>& bytes) -> bool
        {
            return append_bytes(out, bytes.empty() ? nullptr : bytes.data(), bytes.size());
        }

        auto append_string(
            std::vector<uint8_t>& out,
            const std::string& value) -> bool
        {
            return append_bytes(out, reinterpret_cast<const uint8_t*>(value.data()), value.size());
        }

        auto has_remaining(
            const std::vector<uint8_t>& in,
            size_t offset,
            size_t size) -> bool
        {
            return offset <= in.size() && size <= in.size() - offset;
        }

        auto read_u32(
            const std::vector<uint8_t>& in,
            size_t& offset,
            uint32_t& value) -> bool
        {
            if (!has_remaining(in, offset, sizeof(uint32_t)))
                return false;
            value = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                value |= static_cast<uint32_t>(in[offset++]) << shift;
            return true;
        }

        auto read_u64(
            const std::vector<uint8_t>& in,
            size_t& offset,
            uint64_t& value) -> bool
        {
            if (!has_remaining(in, offset, sizeof(uint64_t)))
                return false;
            value = 0;
            for (unsigned shift = 0; shift < 64; shift += 8)
                value |= static_cast<uint64_t>(in[offset++]) << shift;
            return true;
        }

        auto read_bytes(
            const std::vector<uint8_t>& in,
            size_t& offset,
            std::vector<uint8_t>& value) -> bool
        {
            uint32_t size = 0;
            if (!read_u32(in, offset, size))
                return false;
            if (size > max_fake_evidence_field_size || !has_remaining(in, offset, size))
                return false;
            using difference_type = std::vector<uint8_t>::difference_type;
            constexpr auto max_difference = static_cast<size_t>(std::numeric_limits<difference_type>::max());
            if (offset > max_difference || size > max_difference - offset)
                return false;

            value.assign(
                in.begin() + static_cast<difference_type>(offset),
                in.begin() + static_cast<difference_type>(offset + size));
            offset += size;
            return true;
        }

        auto read_string(
            const std::vector<uint8_t>& in,
            size_t& offset,
            std::string& value) -> bool
        {
            std::vector<uint8_t> bytes;
            if (!read_bytes(in, offset, bytes))
                return false;
            value.assign(bytes.begin(), bytes.end());
            return true;
        }

        auto fnv1a_update(
            uint64_t hash,
            const uint8_t* data,
            size_t size) -> uint64_t
        {
            constexpr uint64_t prime = 1099511628211ULL;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= prime;
            }
            return hash;
        }

        auto fnv1a_update(
            uint64_t hash,
            const std::string& value) -> uint64_t
        {
            return fnv1a_update(hash, reinterpret_cast<const uint8_t*>(value.data()), value.size());
        }

        auto fnv1a_update(
            uint64_t hash,
            const std::vector<uint8_t>& value) -> uint64_t
        {
            return fnv1a_update(hash, value.empty() ? nullptr : value.data(), value.size());
        }

        auto append_signed_fake_evidence_fields(
            std::vector<uint8_t>& out,
            const fake_evidence& evidence) -> bool
        {
            // The fake backend is only a development stand-in, but it still
            // signs every field a real verifier would care about: who claimed
            // the identity, which handshake run this is, and the verifier's
            // nonce.
            return append_string(out, evidence.backend_id) && append_string(out, evidence.security_domain_id)
                   && append_string(out, evidence.zone_id) && append_u64(out, evidence.transcript_id)
                   && append_bytes(out, evidence.nonce);
        }

        auto fake_signature(
            const fake_evidence& evidence,
            const std::string& development_key) -> std::optional<uint64_t>
        {
            std::vector<uint8_t> signed_fields;
            if (!append_signed_fake_evidence_fields(signed_fields, evidence))
                return std::nullopt;

            uint64_t hash = 1469598103934665603ULL;
            hash = fnv1a_update(hash, development_key);
            hash = fnv1a_update(hash, signed_fields);
            return hash;
        }

        auto serialise_fake_evidence(const fake_evidence& evidence) -> std::optional<std::vector<uint8_t>>
        {
            std::vector<uint8_t> out;
            if (!append_signed_fake_evidence_fields(out, evidence))
                return std::nullopt;
            if (!append_u64(out, evidence.signature))
                return std::nullopt;
            return out;
        }

        auto parse_fake_evidence(
            const std::vector<uint8_t>& payload,
            fake_evidence& evidence) -> bool
        {
            size_t offset = 0;
            if (!read_string(payload, offset, evidence.backend_id))
                return false;
            if (!read_string(payload, offset, evidence.security_domain_id))
                return false;
            if (!read_string(payload, offset, evidence.zone_id))
                return false;
            if (!read_u64(payload, offset, evidence.transcript_id))
                return false;
            if (!read_bytes(payload, offset, evidence.nonce))
                return false;
            if (!read_u64(payload, offset, evidence.signature))
                return false;
            return offset == payload.size();
        }

        auto reject(std::string reason) -> attestation_verdict
        {
            attestation_verdict verdict;
            verdict.accepted = false;
            verdict.reason = std::move(reason);
            return verdict;
        }

    } // namespace

    fake_backend::fake_backend(std::string development_key)
        : fake_backend(
              std::move(development_key),
              fake_backend_profile{})
    {
    }

    fake_backend::fake_backend(
        std::string development_key,
        fake_backend_profile profile)
        : development_key_(std::move(development_key))
        , profile_(std::move(profile))
    {
    }

    auto fake_backend::backend_id() const -> std::string
    {
        return profile_.backend_id;
    }

    auto fake_backend::level() const -> security_level
    {
        return profile_.level;
    }

    auto fake_backend::produce_evidence(const evidence_binding& binding) const -> cmw
    {
        fake_evidence evidence;
        evidence.backend_id = profile_.backend_id;
        evidence.security_domain_id = binding.subject.security_domain_id;
        evidence.zone_id = binding.subject.zone_id;
        evidence.transcript_id = binding.transcript_id;
        evidence.nonce = binding.nonce;

        cmw result;
        result.media_type = profile_.evidence_media_type;
        result.content_format = profile_.evidence_content_format;
        auto signature = fake_signature(evidence, development_key_);
        if (!signature.has_value())
            return result;
        evidence.signature = signature.value();

        auto payload = serialise_fake_evidence(evidence);
        if (payload.has_value())
            result.payload = std::move(payload.value());
        return result;
    }

    auto fake_backend::verify_evidence(
        const cmw& evidence,
        const evidence_binding& expected_binding,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (evidence.media_type != profile_.evidence_media_type)
            return reject("unsupported fake evidence media type");
        if (evidence.content_format != profile_.evidence_content_format)
            return reject("unsupported fake evidence content format");
        if (!policy.allow_development_evidence)
            return reject("development or simulation evidence is not allowed by policy");
        if (!policy.required_backend_id.empty() && policy.required_backend_id != profile_.backend_id)
            return reject("policy requires a different backend");
        if (security_level_rank(level()) < security_level_rank(policy.minimum_security_level))
            return reject("evidence does not meet the minimum security level");

        fake_evidence parsed;
        if (!parse_fake_evidence(evidence.payload, parsed))
            return reject("malformed fake evidence");
        if (parsed.backend_id != profile_.backend_id)
            return reject("evidence backend id mismatch");
        if (parsed.security_domain_id != expected_binding.subject.security_domain_id
            || parsed.zone_id != expected_binding.subject.zone_id)
            return reject("evidence identity mismatch");
        if (parsed.transcript_id != expected_binding.transcript_id)
            return reject("evidence transcript mismatch");
        if (parsed.nonce != expected_binding.nonce)
            return reject("evidence nonce mismatch");
        auto expected_signature = fake_signature(parsed, development_key_);
        if (!expected_signature.has_value() || parsed.signature != expected_signature.value())
            return reject("evidence signature mismatch");

        attestation_verdict verdict;
        verdict.accepted = true;
        verdict.reason = "development evidence accepted";
        verdict.backend_id = profile_.backend_id;
        verdict.level = level();
        verdict.peer_identity.security_domain_id = parsed.security_domain_id;
        verdict.peer_identity.zone_id = parsed.zone_id;
        return verdict;
    }
} // namespace canopy::security::attestation
