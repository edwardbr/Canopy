/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Host-side (child_transport) implementation for the libcoro_dynamic_library
// transport.  Coroutine-build only.

#ifdef CANOPY_BUILD_COROUTINE

#include <transports/libcoro_dynamic_library/transport.h>
#include <rpc/rpc.h>

namespace rpc::libcoro_dynamic_library
{
    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------
    child_transport::child_transport(std::string name, std::shared_ptr<rpc::service> service, std::string library_path)
        : rpc::transport(name, service)
        , library_path_(std::move(library_path))
    {
    }

    child_transport::~child_transport()
    {
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
            RPC_ERROR("[libcoro_dynamic_library] LoadLibrary failed for {}", library_path_);
            return rpc::error::TRANSPORT_ERROR();
        }
#else
        // RTLD_NODELETE: the DLL's copy of librpc.a may have coroutine frames
        // (e.g. send_object_release) still live on the scheduler when the last
        // proxy is released.  Keeping the code pages mapped lets those frames
        // complete safely; data objects are still freed by reference counting.
        lib_handle_ = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (!lib_handle_)
        {
            RPC_ERROR("[libcoro_dynamic_library] dlopen failed for {}: {}", library_path_, dlerror());
            return rpc::error::TRANSPORT_ERROR();
        }
#endif

        if (!resolve_symbol("canopy_libcoro_dll_create"))
        {
            RPC_ERROR("[libcoro_dynamic_library] canopy_libcoro_dll_create not found in {}", library_path_);
            unload_library();
            return rpc::error::TRANSPORT_ERROR();
        }

        return rpc::error::OK();
    }

    void child_transport::unload_library()
    {
        dll_coro_send_ = nullptr;
        dll_coro_post_ = nullptr;
        dll_coro_try_cast_ = nullptr;
        dll_coro_add_ref_ = nullptr;
        dll_coro_release_ = nullptr;
        dll_coro_object_released_ = nullptr;
        dll_coro_transport_down_ = nullptr;
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
        // Do not sever function pointers here.  send_object_release tasks
        // spawned by the proxy teardown still need to call into the DLL to
        // release their stubs.  The DLL is unloaded only when the
        // child_transport itself is destroyed (after the service drops it).
    }

    void child_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);
    }

    // -------------------------------------------------------------------------
    // inner_connect
    //
    // Step 1 (sync): call canopy_libcoro_dll_create to construct the DLL-side
    //                parent_transport and obtain coroutine function pointers.
    // Step 2 (coro): CO_AWAIT init_fn to run create_child_zone inside the DLL.
    // -------------------------------------------------------------------------
    CORO_TASK(rpc::connect_result)
    child_transport::inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr)
    {
        auto p_this = shared_from_this();
        auto svc = get_service();
        auto scheduler = svc->get_scheduler();

        // Allocate a zone ID for the DLL zone.
        get_new_zone_id_params zone_params;
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT svc->get_new_zone_id(std::move(zone_params));
        if (zone_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("[libcoro_dynamic_library] get_new_zone_id failed: {}", zone_result.error_code);
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

        // --- Step 1: load the shared object ---
        if (int load_err = load_library(); load_err != rpc::error::OK())
            CO_RETURN rpc::connect_result{load_err, {}};

        // --- Step 1 (cont.): call canopy_libcoro_dll_create synchronously ---
        auto create_fn = reinterpret_cast<dll_create_fn>(resolve_symbol("canopy_libcoro_dll_create"));

        dll_create_params create_params{};
        std::string transport_name = get_name();
        create_params.name = transport_name.c_str();
        create_params.dll_zone = adjacent_zone_id;
        create_params.host_zone = get_zone_id();
        create_params.host_ctx = this;
        create_params.host_send = &child_transport::host_inbound_send;
        create_params.host_post = &child_transport::host_inbound_post;
        create_params.host_try_cast = &child_transport::host_inbound_try_cast;
        create_params.host_add_ref = &child_transport::host_inbound_add_ref;
        create_params.host_release = &child_transport::host_inbound_release;
        create_params.host_object_released = &child_transport::host_inbound_object_released;
        create_params.host_transport_down = &child_transport::host_inbound_transport_down;
        create_params.host_get_new_zone_id = &child_transport::host_inbound_get_new_zone_id;
        create_params.host_coro_release_parent = &child_transport::host_coro_release_parent;

        dll_create_result create_result{};
        create_fn(&create_params, &create_result);

        if (!create_result.transport_ctx || !create_result.init_fn)
        {
            RPC_ERROR("[libcoro_dynamic_library] canopy_libcoro_dll_create returned null context");
            unload_library();
            CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        // Store DLL coroutine function pointers
        dll_ctx_ = create_result.transport_ctx;
        dll_coro_send_ = create_result.send_fn;
        dll_coro_post_ = create_result.post_fn;
        dll_coro_try_cast_ = create_result.try_cast_fn;
        dll_coro_add_ref_ = create_result.add_ref_fn;
        dll_coro_release_ = create_result.release_fn;
        dll_coro_object_released_ = create_result.object_released_fn;
        dll_coro_transport_down_ = create_result.transport_down_fn;

        // --- Step 2: CO_AWAIT the DLL's init coroutine ---
        auto init_result = CO_AWAIT create_result.init_fn(dll_ctx_, &input_descr, &scheduler);

        if (init_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("[libcoro_dynamic_library] init_fn failed: {}", init_result.error_code);
            unload_library();
            CO_RETURN rpc::connect_result{init_result.error_code, {}};
        }

        keep_alive_ = p_this;

        set_status(rpc::transport_status::CONNECTED);
        CO_RETURN rpc::connect_result{rpc::error::OK(), init_result.output_descriptor};
    }

    // -------------------------------------------------------------------------
    // Outbound — host → DLL (CO_AWAIT the DLL coroutine function pointer)
    // -------------------------------------------------------------------------
    CORO_TASK(send_result)
    child_transport::outbound_send(send_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_send_ || !dll_ctx_)
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        CO_RETURN CO_AWAIT dll_coro_send_(dll_ctx_, std::move(params));
    }

    CORO_TASK(void)
    child_transport::outbound_post(post_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_post_ || !dll_ctx_)
            CO_RETURN;
        CO_AWAIT dll_coro_post_(dll_ctx_, std::move(params));
    }

    CORO_TASK(standard_result)
    child_transport::outbound_try_cast(try_cast_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_try_cast_ || !dll_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        CO_RETURN CO_AWAIT dll_coro_try_cast_(dll_ctx_, std::move(params));
    }

    CORO_TASK(standard_result)
    child_transport::outbound_add_ref(add_ref_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_add_ref_ || !dll_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        CO_RETURN CO_AWAIT dll_coro_add_ref_(dll_ctx_, std::move(params));
    }

    CORO_TASK(standard_result)
    child_transport::outbound_release(release_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_release_ || !dll_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        CO_RETURN CO_AWAIT dll_coro_release_(dll_ctx_, std::move(params));
    }

    CORO_TASK(void)
    child_transport::outbound_object_released(object_released_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_object_released_ || !dll_ctx_)
            CO_RETURN;
        CO_AWAIT dll_coro_object_released_(dll_ctx_, std::move(params));
    }

    CORO_TASK(void)
    child_transport::outbound_transport_down(transport_down_params params)
    {
        auto p_this = shared_from_this();
        if (!dll_coro_transport_down_ || !dll_ctx_)
            CO_RETURN;
        CO_AWAIT dll_coro_transport_down_(dll_ctx_, std::move(params));
    }

} // namespace rpc::libcoro_dynamic_library

#endif // CANOPY_BUILD_COROUTINE
