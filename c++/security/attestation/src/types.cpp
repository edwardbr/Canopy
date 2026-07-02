/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/types.h>

#include <openssl/crypto.h>

#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        constexpr std::string_view session_id_prefix = "canopy-attestation-session:";
        constexpr std::string_view session_id_format_version = "v2";
        constexpr size_t uint64_decimal_max_digits = std::numeric_limits<uint64_t>::digits10 + 1;
        constexpr size_t size_t_decimal_max_digits = std::numeric_limits<size_t>::digits10 + 1;

        auto session_participant_key(const identity& value) -> std::string
        {
            if (!value.security_domain_id.empty())
                return value.security_domain_id;
            return "/" + value.zone_id;
        }

        template<class T>
        void append_decimal(
            std::string& out,
            T value)
        {
            std::array<char, std::numeric_limits<T>::digits10 + 2> buffer{};
            const auto* const end = buffer.data() + buffer.size();
            auto* cursor = buffer.data() + buffer.size();
            do
            {
                *--cursor = static_cast<char>('0' + (value % 10));
                value /= 10;
            } while (value != 0);
            out.append(cursor, static_cast<size_t>(end - cursor));
        }
    } // namespace

    aead_key_material::~aead_key_material()
    {
        OPENSSL_cleanse(key.data(), key.size());
        OPENSSL_cleanse(nonce_prefix.data(), nonce_prefix.size());
    }

    auto security_level_rank(security_level level) noexcept -> int
    {
        switch (level)
        {
        case security_level::none:
            return 0;
        case security_level::development:
            return 1;
        case security_level::simulation:
            return 2;
        case security_level::hardware_legacy:
            return 3;
        case security_level::hardware:
            return 4;
        }
        return 0;
    }

    auto security_level_name(security_level level) noexcept -> const char*
    {
        switch (level)
        {
        case security_level::none:
            return "none";
        case security_level::development:
            return "development";
        case security_level::simulation:
            return "simulation";
        case security_level::hardware_legacy:
            return "hardware_legacy";
        case security_level::hardware:
            return "hardware";
        }
        return "none";
    }

    auto make_session_id(
        const identity& local,
        const identity& peer,
        uint64_t transcript_id) -> std::string
    {
        auto left = session_participant_key(local);
        auto right = session_participant_key(peer);
        if (right < left)
            std::swap(left, right);

        std::string out;
        out.reserve(
            session_id_prefix.size() + session_id_format_version.size() + left.size() + right.size()
            + (size_t_decimal_max_digits * 2) + uint64_decimal_max_digits + 5);
        // The session id is a readable map key, but it is also fed back into
        // KDF context. Length framing prevents attacker-controlled identity
        // strings containing ':' from aliasing a different participant pair.
        out.append(session_id_prefix);
        out.append(session_id_format_version);
        out.push_back(':');
        append_decimal(out, left.size());
        out.push_back(':');
        out.append(left);
        out.push_back(':');
        append_decimal(out, right.size());
        out.push_back(':');
        out.append(right);
        out.push_back(':');
        append_decimal(out, transcript_id);
        return out;
    }
} // namespace canopy::security::attestation
