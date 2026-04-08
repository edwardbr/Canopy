/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/c_abi/dynamic_library_loader.h>

namespace rpc::c_abi
{
    bool dynamic_library_exports::is_complete() const
    {
        return init && destroy && send && post && try_cast && add_ref && release && object_released && transport_down
               && get_new_zone_id;
    }

    dynamic_library_loader::~dynamic_library_loader()
    {
        unload();
    }

    void* dynamic_library_loader::resolve_symbol(const char* name) const
    {
        if (!handle_)
            return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
        return dlsym(handle_, name);
#endif
    }

    void dynamic_library_loader::clear()
    {
        exports_ = {};
    }

    bool dynamic_library_loader::load(const std::string& path)
    {
        unload();

#if defined(_WIN32)
        handle_ = LoadLibraryA(path.c_str());
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

        if (!handle_)
            return false;

        exports_.init = reinterpret_cast<canopy_dll_init_fn>(resolve_symbol("canopy_dll_init"));
        exports_.destroy = reinterpret_cast<canopy_dll_destroy_fn>(resolve_symbol("canopy_dll_destroy"));
        exports_.send = reinterpret_cast<canopy_dll_send_fn>(resolve_symbol("canopy_dll_send"));
        exports_.post = reinterpret_cast<canopy_dll_post_fn>(resolve_symbol("canopy_dll_post"));
        exports_.try_cast = reinterpret_cast<canopy_dll_try_cast_fn>(resolve_symbol("canopy_dll_try_cast"));
        exports_.add_ref = reinterpret_cast<canopy_dll_add_ref_fn>(resolve_symbol("canopy_dll_add_ref"));
        exports_.release = reinterpret_cast<canopy_dll_release_fn>(resolve_symbol("canopy_dll_release"));
        exports_.object_released
            = reinterpret_cast<canopy_dll_object_released_fn>(resolve_symbol("canopy_dll_object_released"));
        exports_.transport_down
            = reinterpret_cast<canopy_dll_transport_down_fn>(resolve_symbol("canopy_dll_transport_down"));
        exports_.get_new_zone_id
            = reinterpret_cast<canopy_dll_get_new_zone_id_fn>(resolve_symbol("canopy_dll_get_new_zone_id"));

        if (!exports_.is_complete())
        {
            unload();
            return false;
        }

        return true;
    }

    void dynamic_library_loader::unload()
    {
        clear();
        if (!handle_)
            return;

#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }
} // namespace rpc::c_abi
