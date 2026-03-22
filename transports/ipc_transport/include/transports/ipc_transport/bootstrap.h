/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <memory>
#  include <string>
#  include <vector>

#  include <rpc/rpc.h>
#  include <transports/libcoro_spsc_dynamic_dll/dll_abi.h>

namespace rpc::ipc_transport
{
    class queue_pair_bootstrap
    {
    protected:
        std::string mapped_file_;
        rpc::zone child_zone_;

        queue_pair_bootstrap(std::string mapped_file, rpc::zone child_zone);

    public:
        [[nodiscard]] const std::string& mapped_file() const;
        [[nodiscard]] rpc::zone child_zone() const;

        [[nodiscard]] rpc::libcoro_spsc_dynamic_dll::queue_pair* map_queue_pair() const;
        static void unmap_queue_pair(rpc::libcoro_spsc_dynamic_dll::queue_pair* queues);
    };

    class child_host_bootstrap : public queue_pair_bootstrap
    {
        std::string dll_path_;

    public:
        child_host_bootstrap(std::string dll_path, std::string mapped_file, rpc::zone dll_zone);

        [[nodiscard]] static const char* dll_path_arg_name();
        [[nodiscard]] static const char* mapped_file_arg_name();
        [[nodiscard]] static const char* dll_subnet_arg_name();

        [[nodiscard]] static std::shared_ptr<child_host_bootstrap> from_command_line(int argc, char** argv);

        [[nodiscard]] const std::string& dll_path() const;
        [[nodiscard]] rpc::zone dll_zone() const;

        [[nodiscard]] std::vector<std::string> make_command_line() const;
    };

    class child_process_bootstrap : public queue_pair_bootstrap
    {
    public:
        child_process_bootstrap(std::string mapped_file, rpc::zone child_zone);

        [[nodiscard]] static const char* mapped_file_arg_name();
        [[nodiscard]] static const char* child_subnet_arg_name();

        [[nodiscard]] static std::shared_ptr<child_process_bootstrap> from_command_line(int argc, char** argv);
        [[nodiscard]] std::vector<std::string> make_command_line() const;
    };

} // namespace rpc::ipc_transport

#endif // CANOPY_BUILD_COROUTINE
