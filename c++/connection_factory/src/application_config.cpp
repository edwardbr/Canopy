/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/application_config.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <connection_factory/options.h>
#include <connection_factory_config/connection_factory_config_schema.h>
#include <json/config.h>
#include <json/convert.h>

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
#  include <spsc_queue_stream/spsc_queue_stream_config_schema.h>
#  include <streaming/spsc_queue/factory.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
#  include <streaming/secure_stream.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
#  include <security/attestation/backend_factory.h>
#  include <security/attestation/service.h>
#endif

namespace rpc::connection_factory
{
    namespace
    {
        class runtime_config_error final : public std::runtime_error
        {
        public:
            explicit runtime_config_error(std::string message)
                : std::runtime_error(std::move(message))
            {
            }
        };

        [[nodiscard]] auto resolve_path(
            const std::filesystem::path& base_directory,
            const std::string& configured_path) -> std::filesystem::path
        {
            std::filesystem::path path(configured_path);
            if (path.empty() || path.is_absolute() || base_directory.empty())
                return path;
            return base_directory / path;
        }

        [[nodiscard]] auto read_text_file(
            const std::filesystem::path& base_directory,
            const std::string& configured_path) -> std::string
        {
            const auto path = resolve_path(base_directory, configured_path);
            std::ifstream input(path);
            if (!input)
                throw runtime_config_error("failed to open " + path.string());

            std::ostringstream buffer;
            buffer << input.rdbuf();
            return buffer.str();
        }

        [[nodiscard]] bool is_spsc_queue_layer(const rpc::stream_layers::stream_layer_settings& layer)
        {
            return layer.type == "spsc_queue" || layer.type == "spsc";
        }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
        [[nodiscard]] auto collect_spsc_queue_names(const topology_settings& settings) -> std::set<std::string>
        {
            std::set<std::string> names;
            for (const auto& queue : settings.rpc_runtime.spsc_queues)
                names.insert(queue.name);

            for (const auto& connection : settings.connections)
            {
                for (const auto& layer : connection.connection.stream_layers)
                {
                    if (!is_spsc_queue_layer(layer))
                        continue;

                    auto layer_settings
                        = materialise_settings<rpc::spsc_queue_stream::stream_settings>(detail::settings_object(layer));
                    if (layer_settings.error_code != rpc::error::OK())
                    {
                        throw runtime_config_error("invalid spsc_queue settings for connection " + connection.name);
                    }

                    names.insert(
                        layer_settings.settings.queue_pair ? layer_settings.settings.queue_pair.value() : std::string{});
                }
            }
            return names;
        }
#else
        [[nodiscard]] auto configured_spsc_layers(const topology_settings& settings) -> bool
        {
            return std::any_of(
                settings.connections.begin(),
                settings.connections.end(),
                [](const auto& connection)
                {
                    return std::any_of(
                        connection.connection.stream_layers.begin(),
                        connection.connection.stream_layers.end(),
                        is_spsc_queue_layer);
                });
        }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
        [[nodiscard]] auto secure_peer_verification(tls_peer_verification value) -> ::streaming::secure::peer_verification
        {
            switch (value)
            {
            case tls_peer_verification::optional:
                return ::streaming::secure::peer_verification::optional;
            case tls_peer_verification::required:
                return ::streaming::secure::peer_verification::required;
            case tls_peer_verification::none:
            default:
                return ::streaming::secure::peer_verification::none;
            }
        }

        [[nodiscard]] auto configured_pem_or_file(
            const std::filesystem::path& base_directory,
            const std::string& pem,
            const std::string& file) -> std::string
        {
            if (!pem.empty())
                return pem;
            if (!file.empty())
                return read_text_file(base_directory, file);
            return {};
        }

        [[nodiscard]] auto make_tls_client_context(
            const std::filesystem::path& base_directory,
            const tls_runtime_settings& settings) -> std::shared_ptr<::streaming::secure::client_context>
        {
            ::streaming::secure::client_context_options options;
            options.verify_peer = settings.client_verify_peer;

            auto trust_anchor = configured_pem_or_file(
                base_directory, settings.client_trust_anchor_pem, settings.client_trust_anchor_file);
            auto context = trust_anchor.empty()
                               ? std::make_shared<::streaming::secure::client_context>(options)
                               : std::make_shared<::streaming::secure::client_context>(std::move(trust_anchor), options);
            if (!context->is_valid())
                throw runtime_config_error("failed to initialise TLS client context");
            return context;
        }

        [[nodiscard]] auto make_tls_server_context(
            const std::filesystem::path& base_directory,
            const tls_runtime_settings& settings) -> std::shared_ptr<::streaming::secure::context>
        {
            const auto has_inline_credentials
                = !settings.server_certificate_pem.empty() || !settings.server_private_key_pem.empty();
            const auto has_file_credentials
                = !settings.server_certificate_file.empty() || !settings.server_private_key_file.empty();
            if (!has_inline_credentials && !has_file_credentials)
                return {};

            ::streaming::secure::server_context_options options;
            options.verify_peer = secure_peer_verification(settings.server_peer_verification);

            const auto has_trust_anchor
                = !settings.server_trust_anchor_pem.empty() || !settings.server_trust_anchor_file.empty();
            if (!has_inline_credentials && !has_trust_anchor)
            {
                auto context = std::make_shared<::streaming::secure::context>(
                    resolve_path(base_directory, settings.server_certificate_file).string(),
                    resolve_path(base_directory, settings.server_private_key_file).string(),
                    options);
                if (!context->is_valid())
                    throw runtime_config_error("failed to initialise TLS server context");
                return context;
            }

            ::streaming::secure::pem_credentials credentials;
            credentials.certificate = configured_pem_or_file(
                base_directory, settings.server_certificate_pem, settings.server_certificate_file);
            credentials.private_key = configured_pem_or_file(
                base_directory, settings.server_private_key_pem, settings.server_private_key_file);
            credentials.trust_anchor = configured_pem_or_file(
                base_directory, settings.server_trust_anchor_pem, settings.server_trust_anchor_file);
            auto context = std::make_shared<::streaming::secure::context>(credentials, options);
            if (!context->is_valid())
                throw runtime_config_error("failed to initialise TLS server context");
            return context;
        }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
        [[nodiscard]] auto attestation_backend(attestation_backend_kind kind)
            -> canopy::security::attestation::configured_backend_kind
        {
            using canopy::security::attestation::configured_backend_kind;
            switch (kind)
            {
            case attestation_backend_kind::null_backend:
                return configured_backend_kind::null_backend;
            case attestation_backend_kind::sgx_sim_backend:
                return configured_backend_kind::sgx_sim_backend;
            case attestation_backend_kind::fake_backend:
            default:
                return configured_backend_kind::fake_backend;
            }
        }

        [[nodiscard]] auto attestation_level(attestation_security_level level) -> canopy::security::attestation::security_level
        {
            using canopy::security::attestation::security_level;
            switch (level)
            {
            case attestation_security_level::none:
                return security_level::none;
            case attestation_security_level::simulation:
                return security_level::simulation;
            case attestation_security_level::hardware_legacy:
                return security_level::hardware_legacy;
            case attestation_security_level::hardware:
                return security_level::hardware;
            case attestation_security_level::development:
            default:
                return security_level::development;
            }
        }

        void apply_policy(
            canopy::security::attestation::attestation_policy& policy,
            const attestation_policy_settings& settings)
        {
            if (settings.send_local_evidence)
                policy.send_local_evidence = settings.send_local_evidence.value();
            if (settings.require_peer_evidence)
                policy.require_peer_evidence = settings.require_peer_evidence.value();
            if (settings.allow_unattested_peer)
                policy.allow_unattested_peer = settings.allow_unattested_peer.value();
            if (settings.allow_development_evidence)
                policy.allow_development_evidence = settings.allow_development_evidence.value();
            if (settings.minimum_security_level)
                policy.minimum_security_level = attestation_level(settings.minimum_security_level.value());
            if (settings.required_backend_id)
                policy.required_backend_id = settings.required_backend_id.value();
        }

        [[nodiscard]] auto make_attestation_service(const attestation_service_settings& settings)
            -> std::shared_ptr<canopy::security::attestation::attestation_service>
        {
            using canopy::security::attestation::attestation_service;
            using canopy::security::attestation::backend_factory_overrides;
            using canopy::security::attestation::identity;

            const auto backend_kind = attestation_backend(settings.backend);
#  if !defined(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
            if (backend_kind != canopy::security::attestation::configured_backend_kind::null_backend)
            {
                throw runtime_config_error(
                    "fake and sgx_sim attestation backends require development attestation backends to be built");
            }
#  endif

            auto backend = canopy::security::attestation::make_attestation_backend(backend_kind);
            auto options = canopy::security::attestation::make_configured_attestation_service_options(
                identity{settings.identity.enclave_id, settings.identity.zone_id},
                backend_factory_overrides{std::move(backend)});
            apply_policy(options.policy, settings.policy);
            return std::make_shared<attestation_service>(std::move(options));
        }
#endif
    } // namespace

    struct application_runtime::impl
    {
        topology_settings settings;
        std::filesystem::path base_directory;

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
        std::unordered_map<std::string, rpc::spsc_queue::queue_pair> spsc_queues;
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
        std::shared_ptr<::streaming::secure::client_context> tls_client_context;
        std::shared_ptr<::streaming::secure::context> tls_server_context;
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
        std::unordered_map<std::string, std::shared_ptr<canopy::security::attestation::attestation_service>> attestation_services;
#endif

        impl(
            topology_settings runtime_settings,
            std::filesystem::path runtime_base_directory)
            : settings(std::move(runtime_settings))
            , base_directory(std::move(runtime_base_directory))
        {
        }

        void initialise()
        {
            std::set<std::string> connection_names;
            for (const auto& connection : settings.connections)
            {
                if (connection.name.empty())
                    throw runtime_config_error("connection name must not be empty");
                if (!connection_names.insert(connection.name).second)
                    throw runtime_config_error("duplicate connection name: " + connection.name);
            }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
            for (const auto& name : collect_spsc_queue_names(settings))
                spsc_queues.emplace(name, rpc::spsc_queue::queue_pair::create());
#else
            if (!settings.rpc_runtime.spsc_queues.empty() || configured_spsc_layers(settings))
                throw runtime_config_error("SPSC queue runtime settings were provided, but SPSC support is not built");
#endif

            if (settings.rpc_runtime.tls)
            {
#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
                tls_client_context = make_tls_client_context(base_directory, settings.rpc_runtime.tls.value());
                tls_server_context = make_tls_server_context(base_directory, settings.rpc_runtime.tls.value());
#else
                throw runtime_config_error("TLS runtime settings were provided, but TLS support is not built");
#endif
            }

            if (!settings.rpc_runtime.attestation_services.empty())
            {
#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
                for (const auto& service_settings : settings.rpc_runtime.attestation_services)
                {
                    attestation_services.emplace(service_settings.name, make_attestation_service(service_settings));
                }
#else
                throw runtime_config_error(
                    "attestation runtime settings were provided, but attestation support is not built");
#endif
            }
        }

        [[nodiscard]] auto context_for(
            const named_connection_settings& connection,
            std::shared_ptr<rpc::executor> executor) const -> application_context_result
        {
            if (connection.name.empty())
                return {rpc::error::INVALID_DATA(), {}, "connection name must not be empty"};

            layered_connection_context context;

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
            for (const auto& [name, queues] : spsc_queues)
            {
                if (name.empty())
                    context.set_spsc_queues(queues);
                else
                    context.set_dependency_value(queues, name);
            }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING
            if (executor)
                context.set_stream_scheduler(std::move(executor));
#else
            (void)executor;
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
            if (tls_client_context)
                context.set_tls_client_context(tls_client_context);
            if (tls_server_context)
                context.set_tls_server_context(tls_server_context);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
            for (const auto& [name, service] : attestation_services)
            {
                if (name.empty())
                    context.set_attestation_service(service);
                else
                    context.register_attestation_service(name, service);
            }
#endif

            return {rpc::error::OK(), std::move(context), {}};
        }
    };

    application_runtime::application_runtime(
        topology_settings settings,
        std::filesystem::path base_directory)
        : impl_(
              std::make_shared<impl>(
                  std::move(settings),
                  std::move(base_directory)))
    {
        impl_->initialise();
    }

    application_runtime::~application_runtime() = default;

    auto application_runtime::settings() const -> const topology_settings&
    {
        return impl_->settings;
    }

    auto application_runtime::find_connection(std::string_view name) const -> const named_connection_settings*
    {
        const auto& connections = impl_->settings.connections;
        const auto found = std::find_if(
            connections.begin(), connections.end(), [name](const auto& connection) { return connection.name == name; });
        return found == connections.end() ? nullptr : &*found;
    }

    auto application_runtime::context_for(
        const named_connection_settings& connection,
        std::shared_ptr<rpc::executor> executor) const -> application_context_result
    {
        return impl_->context_for(connection, std::move(executor));
    }

    auto make_application_runtime(
        topology_settings settings,
        std::filesystem::path base_directory) -> application_runtime_result
    {
        try
        {
            auto runtime = std::make_shared<application_runtime>(std::move(settings), std::move(base_directory));
            return {rpc::error::OK(), std::move(runtime), {}};
        }
        catch (const std::exception& error)
        {
            return {rpc::error::INVALID_DATA(), {}, error.what()};
        }
    }

    auto load_application_runtime(const std::filesystem::path& path) -> application_runtime_result
    {
        try
        {
            return make_application_runtime(
                json::v1::convert::from_json_object<topology_settings>(json::v1::parse_file(path)), path.parent_path());
        }
        catch (const std::exception& error)
        {
            return {rpc::error::INVALID_DATA(), {}, error.what()};
        }
    }
} // namespace rpc::connection_factory
