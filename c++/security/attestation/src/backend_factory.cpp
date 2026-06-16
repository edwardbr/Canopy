/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backend_factory.h>

#include <security/attestation/backends/null/null_backend.h>

#ifdef CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS
#  include <security/attestation/backends/fake/fake_backend.h>
#endif

#include <exception>
#include <memory>
#include <utility>

#if defined(CANOPY_PRODUCTION_RELEASE) && defined(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
#  error "Production releases must not build fake attestation backend implementations"
#endif

#if !defined(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)                                                           \
    && defined(CANOPY_ATTESTATION_BACKEND_FAKE)
#  error "Fake attestation backend selected while development backends are disabled"
#endif

namespace canopy::security::attestation
{
    namespace
    {
        constexpr const char* fake_backend_name = "fake";

        [[noreturn]] void terminate_for_invalid_backend_configuration() noexcept
        {
            std::terminate();
        }

        [[nodiscard]] auto default_policy_for_backend(const attestation_backend& backend) -> attestation_policy
        {
            attestation_policy policy;
            policy.required_backend_id = backend.backend_id();

            if (policy.required_backend_id == null_backend_id)
            {
                policy.send_local_evidence = false;
                policy.require_peer_evidence = false;
                policy.allow_unattested_peer = true;
                policy.allow_development_evidence = false;
                policy.minimum_security_level = security_level::none;
            }
            else if (policy.required_backend_id == fake_backend_name)
            {
                policy.send_local_evidence = true;
                policy.require_peer_evidence = true;
                policy.allow_unattested_peer = false;
                policy.allow_development_evidence = true;
                policy.minimum_security_level = security_level::development;
            }
            else
            {
                policy.send_local_evidence = true;
                policy.require_peer_evidence = true;
                policy.allow_unattested_peer = false;
                policy.allow_development_evidence = false;
                policy.minimum_security_level = backend.level();
            }

            return policy;
        }
    } // namespace

    auto attestation_backend_kind_name(configured_backend_kind kind) noexcept -> const char*
    {
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            return null_backend_id;
        case configured_backend_kind::fake_backend:
            return fake_backend_name;
        }
        terminate_for_invalid_backend_configuration();
    }

    auto configured_attestation_backend_kind() noexcept -> configured_backend_kind
    {
#ifdef CANOPY_ATTESTATION_BACKEND_NULL
        return configured_backend_kind::null_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_FAKE)
        return configured_backend_kind::fake_backend;
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
#ifdef CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS
            return std::make_shared<fake_backend>();
#else
            terminate_for_invalid_backend_configuration();
#endif
        }
        terminate_for_invalid_backend_configuration();
    }

    auto make_configured_attestation_backend() -> std::shared_ptr<attestation_backend>
    {
        return make_configured_attestation_backend({});
    }

    auto make_configured_attestation_backend(backend_factory_overrides overrides) -> std::shared_ptr<attestation_backend>
    {
        if (overrides.backend)
            return std::move(overrides.backend);
        return make_attestation_backend(configured_attestation_backend_kind());
    }

    auto make_configured_attestation_service_options(
        identity local_identity,
        backend_factory_overrides overrides) -> attestation_service_options
    {
        auto kind = configured_attestation_backend_kind();

        attestation_service_options options;
        options.local_identity = std::move(local_identity);
        if (overrides.backend)
            options.backend = std::move(overrides.backend);
        else
            options.backend = make_attestation_backend(kind);

        options.policy = default_policy_for_backend(*options.backend);
        return options;
    }
} // namespace canopy::security::attestation
