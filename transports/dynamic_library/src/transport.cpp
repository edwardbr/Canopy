/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Host-side (child_transport) implementation.
// This file is part of transport_dynamic_library and is NOT linked into
// DLL payloads.
//
// dlclose timing:
//   The shared object must NOT be unloaded while DLL code is still on the
//   call stack.  Two safe points exist:
//     1. on_destination_count_zero() — all host-side proxies are gone, so no
//        DLL->host callbacks can be in flight.
//     2. The destructor — last shared_ptr to this transport is gone.
//   set_status(DISCONNECTED) only nulls out function pointers so that
//   subsequent outbound calls fail gracefully; it never calls dlclose.

#include <transports/dynamic_library/transport.h>
#include <rpc/rpc.h>

namespace rpc::dynamic_library
{
    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------
    child_transport::child_transport(
        std::string name, std::shared_ptr<rpc::service> service, std::string library_path)
        : rpc::transport(name, service)
        , library_path_(std::move(library_path))
    {
        // Status remains at its base-class default (CONNECTING) until inner_connect
        // succeeds and explicitly transitions to CONNECTED.
    }

    child_transport::~child_transport()
    {
        // Final safety net: if the transport is destroyed without going through
        // on_destination_count_zero (e.g. after an init failure) clean up now.
        if (dll_ctx_ && dll_destroy_)
        {
            dll_destroy_(dll_ctx_);
            dll_ctx_ = nullptr;
        }
        unload_library();
    }

    // -------------------------------------------------------------------------
    // Symbol resolution
    // -------------------------------------------------------------------------
    void* child_transport::resolve_symbol(const char* sym_name) const
    {
        if (!lib_handle_)
            return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib_handle_), sym_name));
#else
        return dlsym(lib_handle_, sym_name);
#endif
    }

    int child_transport::load_library()
    {
        RPC_ASSERT(!lib_handle_);

#if defined(_WIN32)
        lib_handle_ = LoadLibraryA(library_path_.c_str());
        if (!lib_handle_)
        {
            RPC_ERROR("[dynamic_library] LoadLibrary failed for {}", library_path_);
            return rpc::error::TRANSPORT_ERROR();
        }
#else
        // RTLD_LOCAL: keep DLL symbols private; RTLD_NOW: fail fast on missing deps.
        lib_handle_ = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib_handle_)
        {
            RPC_ERROR("[dynamic_library] dlopen failed for {}: {}", library_path_, dlerror());
            return rpc::error::TRANSPORT_ERROR();
        }
#endif

        dll_init_ = reinterpret_cast<dll_init_fn>(resolve_symbol("canopy_dll_init"));
        dll_destroy_ = reinterpret_cast<dll_destroy_fn>(resolve_symbol("canopy_dll_destroy"));
        dll_send_ = reinterpret_cast<dll_send_fn>(resolve_symbol("canopy_dll_send"));
        dll_post_ = reinterpret_cast<dll_post_fn>(resolve_symbol("canopy_dll_post"));
        dll_try_cast_ = reinterpret_cast<dll_try_cast_fn>(resolve_symbol("canopy_dll_try_cast"));
        dll_add_ref_ = reinterpret_cast<dll_add_ref_fn>(resolve_symbol("canopy_dll_add_ref"));
        dll_release_ = reinterpret_cast<dll_release_fn>(resolve_symbol("canopy_dll_release"));
        dll_object_released_
            = reinterpret_cast<dll_object_released_fn>(resolve_symbol("canopy_dll_object_released"));
        dll_transport_down_
            = reinterpret_cast<dll_transport_down_fn>(resolve_symbol("canopy_dll_transport_down"));
        dll_get_new_zone_id_
            = reinterpret_cast<dll_get_new_zone_id_fn>(resolve_symbol("canopy_dll_get_new_zone_id"));

        if (!dll_init_ || !dll_destroy_ || !dll_send_ || !dll_post_ || !dll_try_cast_ || !dll_add_ref_
            || !dll_release_ || !dll_object_released_ || !dll_transport_down_ || !dll_get_new_zone_id_)
        {
            RPC_ERROR("[dynamic_library] one or more canopy_dll_* entry points missing in {}", library_path_);
            unload_library();
            return rpc::error::TRANSPORT_ERROR();
        }

        return rpc::error::OK();
    }

    void child_transport::unload_library()
    {
        // Null out all pointers before closing so any stale callers fail visibly.
        dll_init_ = nullptr;
        dll_destroy_ = nullptr;
        dll_send_ = nullptr;
        dll_post_ = nullptr;
        dll_try_cast_ = nullptr;
        dll_add_ref_ = nullptr;
        dll_release_ = nullptr;
        dll_object_released_ = nullptr;
        dll_transport_down_ = nullptr;
        dll_get_new_zone_id_ = nullptr;
        dll_ctx_ = nullptr;

        if (lib_handle_)
        {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(lib_handle_));
#else
            dlclose(lib_handle_);
#endif
            lib_handle_ = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    // Lifetime management
    // -------------------------------------------------------------------------
    void child_transport::on_destination_count_zero()
    {
        // All host-side proxies into the DLL zone are gone.  No DLL code can be
        // on the call stack at this point, so it is safe to destroy and unload.
        if (dll_ctx_ && dll_destroy_)
        {
            dll_destroy_(dll_ctx_);
            dll_ctx_ = nullptr;
        }
        unload_library();
        set_status(rpc::transport_status::DISCONNECTED);
    }

    void child_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);

        if (status == rpc::transport_status::DISCONNECTED)
        {
            // Sever the function-pointer bridge so that subsequent outbound
            // calls fail gracefully.  Do NOT call dlclose here because this
            // method may be reached from inside a DLL callback (e.g. via
            // cb_transport_down -> inbound_transport_down -> proxy release).
            // The actual unload is deferred to on_destination_count_zero or
            // the destructor, both of which are guaranteed to run after any
            // DLL callback has returned.
            dll_init_ = nullptr;
            dll_send_ = nullptr;
            dll_post_ = nullptr;
            dll_try_cast_ = nullptr;
            dll_add_ref_ = nullptr;
            dll_release_ = nullptr;
            dll_object_released_ = nullptr;
            dll_transport_down_ = nullptr;
            dll_get_new_zone_id_ = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    // inner_connect
    // -------------------------------------------------------------------------
    CORO_TASK(rpc::connect_result)
    child_transport::inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr)
    {
        auto svc = get_service();

        // Allocate a zone ID for the DLL zone
        get_new_zone_id_params zone_params;
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT svc->get_new_zone_id(std::move(zone_params));
        if (zone_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("[dynamic_library] get_new_zone_id failed: {}", zone_result.error_code);
            CO_RETURN rpc::connect_result{zone_result.error_code, {}};
        }

        rpc::zone adjacent_zone_id = zone_result.zone_id;
        set_adjacent_zone_id(adjacent_zone_id);
        svc->add_transport(adjacent_zone_id, shared_from_this());

        if (stub)
        {
            auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id);
            if (ret != rpc::error::OK())
                CO_RETURN rpc::connect_result{ret, {}};
        }

        // Load the shared object and resolve entry points
        if (int load_err = load_library(); load_err != rpc::error::OK())
            CO_RETURN rpc::connect_result{load_err, {}};

        // Build the init parameters struct.
        // Store the name in a local so init_params.name doesn't dangle.
        std::string transport_name = get_name();
        dll_init_params init_params{};
        init_params.name = transport_name.c_str();
        init_params.host_zone = get_zone_id();
        init_params.dll_zone = adjacent_zone_id;
        init_params.input_descr = &input_descr;
        init_params.host_ctx = this;
        init_params.host_send = &child_transport::cb_send;
        init_params.host_post = &child_transport::cb_post;
        init_params.host_try_cast = &child_transport::cb_try_cast;
        init_params.host_add_ref = &child_transport::cb_add_ref;
        init_params.host_release = &child_transport::cb_release;
        init_params.host_object_released = &child_transport::cb_object_released;
        init_params.host_transport_down = &child_transport::cb_transport_down;
        init_params.host_get_new_zone_id = &child_transport::cb_get_new_zone_id;
        init_params.dll_ctx = nullptr;

        int init_err = dll_init_(&init_params);
        if (init_err != rpc::error::OK())
        {
            RPC_ERROR("[dynamic_library] canopy_dll_init failed: {}", init_err);
            unload_library();
            CO_RETURN rpc::connect_result{init_err, {}};
        }

        dll_ctx_ = init_params.dll_ctx;
        set_status(rpc::transport_status::CONNECTED);

        CO_RETURN rpc::connect_result{rpc::error::OK(), init_params.output_obj};
    }

    // -------------------------------------------------------------------------
    // Outbound methods — host sends to DLL
    // -------------------------------------------------------------------------
    CORO_TASK(send_result)
    child_transport::outbound_send(send_params params)
    {
        if (!dll_send_ || !dll_ctx_)
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        send_result result;
        dll_send_(dll_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    child_transport::outbound_post(post_params params)
    {
        if (!dll_post_ || !dll_ctx_)
            CO_RETURN;

        dll_post_(dll_ctx_, &params);
    }

    CORO_TASK(standard_result)
    child_transport::outbound_try_cast(try_cast_params params)
    {
        if (!dll_try_cast_ || !dll_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        standard_result result;
        dll_try_cast_(dll_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    child_transport::outbound_add_ref(add_ref_params params)
    {
        if (!dll_add_ref_ || !dll_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        standard_result result;
        dll_add_ref_(dll_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    child_transport::outbound_release(release_params params)
    {
        if (!dll_release_ || !dll_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        standard_result result;
        dll_release_(dll_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    child_transport::outbound_object_released(object_released_params params)
    {
        if (!dll_object_released_ || !dll_ctx_)
            CO_RETURN;

        dll_object_released_(dll_ctx_, &params);
    }

    CORO_TASK(void)
    child_transport::outbound_transport_down(transport_down_params params)
    {
        if (!dll_transport_down_ || !dll_ctx_)
            CO_RETURN;

        dll_transport_down_(dll_ctx_, &params);
    }

    // -------------------------------------------------------------------------
    // Static host-callback shims
    // These are passed as function pointers to canopy_dll_init and are called
    // by the DLL's parent_transport to reach the host inbound handlers.
    // They must NOT trigger dlclose (the DLL stack is active when they run).
    // -------------------------------------------------------------------------
    int child_transport::cb_send(void* host_ctx, rpc::send_params* params, rpc::send_result* result)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        *result = t->inbound_send(std::move(*params));
        return result->error_code;
    }

    void child_transport::cb_post(void* host_ctx, rpc::post_params* params)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        t->inbound_post(std::move(*params));
    }

    int child_transport::cb_try_cast(
        void* host_ctx, rpc::try_cast_params* params, rpc::standard_result* result)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        *result = t->inbound_try_cast(std::move(*params));
        return result->error_code;
    }

    int child_transport::cb_add_ref(
        void* host_ctx, rpc::add_ref_params* params, rpc::standard_result* result)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        *result = t->inbound_add_ref(std::move(*params));
        return result->error_code;
    }

    int child_transport::cb_release(
        void* host_ctx, rpc::release_params* params, rpc::standard_result* result)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        *result = t->inbound_release(std::move(*params));
        return result->error_code;
    }

    void child_transport::cb_object_released(void* host_ctx, rpc::object_released_params* params)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        t->inbound_object_released(std::move(*params));
    }

    void child_transport::cb_transport_down(void* host_ctx, rpc::transport_down_params* params)
    {
        // Propagate the DLL-initiated transport-down notification to the host
        // service so it can clean up its proxy objects.  We do NOT trigger
        // dlclose here — the DLL code that called set_status(DISCONNECTED) is
        // still on the stack.  The unload will happen in on_destination_count_zero
        // once all host-side refs have been released.
        auto* t = static_cast<child_transport*>(host_ctx);
        t->inbound_transport_down(std::move(*params));
    }

    int child_transport::cb_get_new_zone_id(
        void* host_ctx, rpc::get_new_zone_id_params* params, rpc::new_zone_id_result* result)
    {
        auto* t = static_cast<child_transport*>(host_ctx);
        auto svc = t->get_service();
        if (!svc)
        {
            *result = rpc::new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
            return result->error_code;
        }
        *result = svc->get_new_zone_id(std::move(*params));
        return result->error_code;
    }

} // namespace rpc::dynamic_library
