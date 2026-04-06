/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <c_abi/dynamic_library/canopy_dynamic_library.h>

#include <string>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace rpc::c_abi
{
    struct dynamic_library_exports
    {
        canopy_dll_init_fn init = nullptr;
        canopy_dll_destroy_fn destroy = nullptr;
        canopy_dll_send_fn send = nullptr;
        canopy_dll_post_fn post = nullptr;
        canopy_dll_try_cast_fn try_cast = nullptr;
        canopy_dll_add_ref_fn add_ref = nullptr;
        canopy_dll_release_fn release = nullptr;
        canopy_dll_object_released_fn object_released = nullptr;
        canopy_dll_transport_down_fn transport_down = nullptr;
        canopy_dll_get_new_zone_id_fn get_new_zone_id = nullptr;

        bool is_complete() const;
    };

    class dynamic_library_loader
    {
        void* handle_ = nullptr;
        dynamic_library_exports exports_{};

        void* resolve_symbol(const char* name) const;
        void clear();

    public:
        dynamic_library_loader() = default;
        ~dynamic_library_loader();

        dynamic_library_loader(const dynamic_library_loader&) = delete;
        dynamic_library_loader& operator=(const dynamic_library_loader&) = delete;

        bool load(const std::string& path);
        void unload();

        const dynamic_library_exports& exports() const { return exports_; }
        bool is_loaded() const { return handle_ != nullptr && exports_.is_complete(); }
    };
} // namespace rpc::c_abi
