/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <json/json_dom.h>
#include <rpc/rpc.h>

namespace rpc::module
{
    // Object modules declare what they can instantiate once, independent of
    // whether the containing binary is a blocking DLL, coroutine DLL, or
    // another module runtime. The runtime-specific
    // <rpc_objects/object_registration.h> header supplies the matching
    // rpc::object_module_init_params type and rpc::register_object adapter.
    //
    // In that sense canopy_module_init plus register_object is deliberately a
    // static-polymorphic module hook: the module code sees one shape, while
    // the selected transport maps it to its own init ABI and lifetime rules.

    // Per-module configuration is represented as JSON because the current
    // object-module transports receive module startup configuration from JSON
    // config files. This is why this helper lives in the modules subcomponent
    // instead of rpc/internal/service.h: the generic service machinery should
    // not casually grow a dependency on the JSON DOM layer.
    using module_settings_object = json::v1::object;

    // Settings for the named applications inside one loaded module instance.
    // This must never be a process-wide application map: a module must not see
    // another loaded module's application configuration.
    //
    // The map key is the module-local application name, and the value is that
    // application's JSON settings.
    using startup_applications = std::map<std::string, module_settings_object>;

    // The factory context can be copied into lambdas and retained by object
    // implementations. Store immutable shared state so caller-owned JSON
    // objects are not borrowed past their lifetime.
    using shared_module_settings = std::shared_ptr<const module_settings_object>;
    using shared_startup_applications = std::shared_ptr<const startup_applications>;

    // Default settings are intentionally stable references. They are used when
    // a transport did not provide settings for the selected module.
    inline const json::v1::object& empty_module_settings()
    {
        static const json::v1::object settings{json::v1::map{}};
        return settings;
    }

    // Empty module-local application map for callers that want to inspect all
    // startup applications through object_factory_context::applications().
    inline const startup_applications& empty_startup_applications()
    {
        static const startup_applications applications;
        return applications;
    }

    // Copy module settings into immutable shared storage at the boundary where
    // the transport builds an object factory. After this, the caller may destroy
    // or mutate its original JSON object without affecting the factory context.
    //
    // A null pointer means no settings were safely supplied for this runtime.
    // Local in-process transports can pass typed objects directly. Non-local
    // runtime boundaries such as shared objects and C ABI modules
    // should serialize first and parse on the receiving side.
    inline shared_module_settings share_module_settings(const module_settings_object* settings)
    {
        if (!settings)
            return {};
        return std::make_shared<module_settings_object>(*settings);
    }

    // Startup applications are optional. A null pointer means "no application
    // map was supplied"; a non-null pointer is copied so factory lambdas do not
    // keep raw references into request-local transport state.
    inline shared_startup_applications share_startup_applications(const startup_applications* applications)
    {
        if (!applications)
            return {};
        return std::make_shared<startup_applications>(*applications);
    }

    // Look up configuration for one named application inside the currently
    // loaded module. The map is module-local; callers must not pass a map that
    // contains applications belonging to another module or the host executable.
    inline const module_settings_object* find_application_settings(
        const startup_applications* applications,
        const std::string& name)
    {
        if (!applications)
            return nullptr;

        auto application = applications->find(name);
        if (application == applications->end())
            return nullptr;

        return &application->second;
    }

    // Metadata passed to the concrete object factory when a local object is
    // created behind an RPC interface. The context is deliberately small and
    // copyable: factories receive it by value so they can keep it if the object
    // implementation wants to read settings later.
    struct object_factory_context
    {
        // Logical module-local application name used by the transport. This is
        // also the key normally used to look up this application in
        // startup_applications.
        std::string name;

        // Optional startup application map for this module instance only. Most
        // applications only need their own settings(), but a module may inspect
        // sibling applications inside the same loaded module. It must not
        // contain application settings from other modules.
        shared_startup_applications module_startup_applications;

        // Module-global settings supplied to this loaded module instance. This
        // is separate from settings() so object implementations can read
        // both the selected application settings and the enclosing module's
        // global settings.
        shared_module_settings module_global_settings;

        // Return this object's application settings when the module startup
        // configuration contains an entry matching name; otherwise return
        // module-global settings. Interface fingerprints select the factory
        // that runs, while the module-local application name selects settings.
        [[nodiscard]] const module_settings_object& settings() const
        {
            if (auto* application_settings = find_application_settings(module_startup_applications.get(), name))
                return *application_settings;
            return global_settings();
        }

        // Return module-global settings, or an empty JSON object when no
        // module-global settings were supplied.
        [[nodiscard]] const module_settings_object& global_settings() const
        {
            return module_global_settings ? *module_global_settings : empty_module_settings();
        }

        // Return this module instance's application settings map, or an empty
        // map when the transport did not provide one.
        [[nodiscard]] const startup_applications& applications() const
        {
            return module_startup_applications ? *module_startup_applications : empty_startup_applications();
        }
    };

    // Build the safe copyable context from the transport-provided inputs. The
    // inputs are borrowed only for the duration of this call; the returned
    // context owns shared copies.
    inline object_factory_context make_object_factory_context(
        std::string name,
        const module_settings_object* module_global_settings,
        const startup_applications* startup_applications = nullptr)
    {
        return {std::move(name),
            share_startup_applications(startup_applications),
            share_module_settings(module_global_settings)};
    }

    // Common shape for object implementation factories.
    //
    // Remote is the interface exposed by the parent/host side.
    // Local is the interface implemented by the object module.
    //
    // The transport supplies the remote proxy, the service that will own the
    // local object, and the context described above. The factory returns the
    // concrete local interface object plus an error code.
    template<class Remote, class Local>
    using object_factory = std::function<CORO_TASK(rpc::service_connect_result<Local>)(
        rpc::shared_ptr<Remote>, std::shared_ptr<rpc::service>, object_factory_context)>;

    // Type-erasure helper. This keeps call sites from spelling the full
    // std::function type and lets lambdas/functions be converted consistently.
    template<
        class Remote,
        class Local,
        class Factory>
    object_factory<
        Remote,
        Local>
    make_object_factory(Factory&& factory)
    {
        return object_factory<Remote, Local>(std::forward<Factory>(factory));
    }

    // Convenience adapter for the common case where the implementation does not
    // need the parent/host remote interface or module settings. The supplied
    // LocalFactory only receives the owning rpc::service and returns Local.
    template<
        class Remote,
        class Local,
        class LocalFactory>
    object_factory<
        Remote,
        Local>
    make_object_factory_with_service(LocalFactory local_factory)
    {
        return [local_factory = std::move(local_factory)](
                   rpc::shared_ptr<Remote> remote,
                   std::shared_ptr<rpc::service> service,
                   object_factory_context context) mutable -> CORO_TASK(rpc::service_connect_result<Local>)
        {
            (void)remote;
            (void)context;
            auto local = local_factory(std::move(service));
            CO_RETURN rpc::service_connect_result<Local>{rpc::error::OK(), std::move(local)};
        };
    }

    // Adapt a generic object_factory to the shape expected by
    // rpc::child_service::create_child_zone().
    //
    // This is used by transports that create a child service for the module,
    // such as blocking_dll. The wrapper converts
    // child_service to its base service type and injects the safe context.
    template<
        class Remote,
        class Local>
    auto make_child_service_factory(
        std::string name,
        const json::v1::object* module_global_settings,
        object_factory<
            Remote,
            Local> factory,
        const startup_applications* startup_applications = nullptr)
        -> std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            rpc::shared_ptr<Remote>,
            std::shared_ptr<rpc::child_service>)>
    {
        auto context = make_object_factory_context(std::move(name), module_global_settings, startup_applications);
        return [context = std::move(context), factory = std::move(factory)](
                   rpc::shared_ptr<Remote> remote,
                   std::shared_ptr<rpc::child_service> service) mutable -> CORO_TASK(rpc::service_connect_result<Local>)
        {
            auto service_base = std::static_pointer_cast<rpc::service>(service);
            CO_RETURN CO_AWAIT factory(std::move(remote), std::move(service_base), context);
        };
    }

    // Adapt a generic object_factory to transports that already pass a service
    // rather than a child_service. This is used by shared-scheduler
    // registration paths where the transport setup already controls the
    // service lifetime.
    template<
        class Remote,
        class Local>
    auto make_service_factory(
        std::string name,
        const json::v1::object* module_global_settings,
        const startup_applications* startup_applications,
        object_factory<
            Remote,
            Local> factory)
        -> std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            rpc::shared_ptr<Remote>,
            std::shared_ptr<rpc::service>)>
    {
        auto context = make_object_factory_context(std::move(name), module_global_settings, startup_applications);
        return [context = std::move(context), factory = std::move(factory)](
                   rpc::shared_ptr<Remote> remote,
                   std::shared_ptr<rpc::service> service) mutable -> CORO_TASK(rpc::service_connect_result<Local>)
        { CO_RETURN CO_AWAIT factory(std::move(remote), std::move(service), context); };
    }

} // namespace rpc::module
