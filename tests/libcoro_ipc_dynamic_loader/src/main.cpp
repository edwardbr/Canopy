/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <chrono>
#  include <cstdlib>
#  include <fcntl.h>
#  include <iostream>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <thread>
#  include <unistd.h>

#  include <transports/libcoro_ipc_dynamic_dll/loaded_library.h>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: canopy_libcoro_ipc_loader <dll-path> <mapped-queue-file> <dll-subnet>\n";
        return 1;
    }

    int fd = ::open(argv[2], O_RDWR, 0600);
    if (fd < 0)
        return 2;

    auto* queues = static_cast<rpc::libcoro_ipc_dynamic_dll::queue_pair*>(
        ::mmap(nullptr, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ::close(fd);
    if (queues == MAP_FAILED)
        return 3;

    rpc::zone host_zone = rpc::DEFAULT_PREFIX;
    auto dll_zone_address = rpc::DEFAULT_PREFIX;
    char* end = nullptr;
    auto dll_subnet = std::strtoull(argv[3], &end, 10);
    if (!end || *end != '\0')
        return 5;

    [[maybe_unused]] bool ok = dll_zone_address.set_subnet(dll_subnet);
    RPC_ASSERT(ok);
    rpc::zone dll_zone = dll_zone_address;

    auto loaded = rpc::libcoro_ipc_dynamic_dll::loaded_library::load(
        argv[1], "libcoro_ipc_loader", dll_zone, host_zone, &queues->dll_to_host, &queues->host_to_dll);
    if (!loaded)
        return 4;

    std::cout << "loader: loaded dll zone " << dll_zone.get_subnet() << '\n';
    loaded->wait_until_expired(std::chrono::seconds(30));
    std::cout << "loader: parent expired for dll zone " << dll_zone.get_subnet() << '\n';
    loaded->stop();
    std::cout << "loader: stopped dll zone " << dll_zone.get_subnet() << '\n';

    ::munmap(queues, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair));
    return 0;
}

#endif // CANOPY_BUILD_COROUTINE
