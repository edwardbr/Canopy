/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <chrono>
#  include <sys/stat.h>
#  include <thread>
#  include <unistd.h>

#  include <transports/ipc_transport/bootstrap.h>
#  include <transports/libcoro_spsc_dynamic_dll/loaded_library.h>

int main(
    int argc,
    char** argv)
{
    auto bootstrap = rpc::ipc_transport::child_host_bootstrap::from_command_line(argc, argv);
    if (!bootstrap)
        return 2;

    auto* queues = bootstrap->map_queue_pair();
    if (!queues)
        return 3;

    rpc::zone host_zone = rpc::DEFAULT_PREFIX;
    rpc::zone dll_zone = bootstrap->dll_zone();

    auto loaded = rpc::libcoro_spsc_dynamic_dll::loaded_library::load(
        bootstrap->dll_path(),
        "ipc_child_host_process",
        dll_zone,
        host_zone,
        &queues->dll_to_host,
        &queues->host_to_dll,
        bootstrap->scheduler_thread_count());
    if (!loaded)
        return 4;

    loaded->wait_until_expired(std::chrono::seconds(30));
    loaded->stop();

    rpc::ipc_transport::child_host_bootstrap::unmap_queue_pair(queues);
    return 0;
}

#endif // CANOPY_BUILD_COROUTINE
