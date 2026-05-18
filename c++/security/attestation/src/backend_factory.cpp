/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backend_factory.h>

#include <security/attestation/fake_backend.h>
#include <security/attestation/null_backend.h>

#include <memory>
#include <utility>

namespace canopy::security::attestation
{
    auto parse_attestation_backend_kind(std::string_view name) noexcept -> std::optional<configured_backend_kind>
    {
        if (name == "FAKE" || name == "fake")
            return configured_backend_kind::fake_backend;
        if (name == "NULL" || name == "null")
            return configured_backend_kind::null_backend;
        return std::nullopt;
    }

    auto attestation_backend_kind_name(configured_backend_kind kind) noexcept -> const char*
    {
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            return null_backend_id;
        case configured_backend_kind::fake_backend:
            return fake_backend_id;
        }
        return fake_backend_id;
    }

    auto configured_attestation_backend_kind() noexcept -> configured_backend_kind
    {
#ifdef CANOPY_ATTESTATION_BACKEND_NULL
        return configured_backend_kind::null_backend;
#else
        return configured_backend_kind::fake_backend;
#endif
    }

    auto configured_attestation_backend_name() noexcept -> const char*
    {
        return attestation_backend_kind_name(configured_attestation_backend_kind());
    }

    auto make_attestation_backend(configured_backend_kind kind) -> std::shared_ptr<attestation_backend>
    {
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            return std::make_shared<null_backend>();
        case configured_backend_kind::fake_backend:
            return std::make_shared<fake_backend>();
        }
        return std::make_shared<fake_backend>();
    }

    auto make_configured_attestation_backend() -> std::shared_ptr<attestation_backend>
    {
        return make_attestation_backend(configured_attestation_backend_kind());
    }

    auto make_default_attestation_service_options(identity local_identity) -> attestation_service_options
    {
        auto kind = configured_attestation_backend_kind();

        attestation_service_options options;
        options.local_identity = std::move(local_identity);
        options.backend = make_attestation_backend(kind);

        options.policy = attestation_policy{};
        options.policy.required_backend_id = attestation_backend_kind_name(kind);
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            options.policy.send_local_evidence = false;
            options.policy.require_peer_evidence = false;
            options.policy.allow_unattested_peer = true;
            options.policy.allow_development_evidence = false;
            options.policy.minimum_security_level = security_level::none;
            break;
        case configured_backend_kind::fake_backend:
            options.policy.send_local_evidence = true;
            options.policy.require_peer_evidence = true;
            options.policy.allow_unattested_peer = false;
            options.policy.allow_development_evidence = true;
            options.policy.minimum_security_level = security_level::development;
            break;
        }
        return options;
    }
} // namespace canopy::security::attestation
