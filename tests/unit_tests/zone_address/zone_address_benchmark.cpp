/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

#include <rpc/rpc.h>

namespace
{
    template<typename Fn> void run_benchmark(std::string_view name, std::uint64_t iterations, Fn&& fn)
    {
        auto start = std::chrono::steady_clock::now();
        volatile std::uint64_t sink = fn(iterations);
        auto end = std::chrono::steady_clock::now();

        auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        auto per_iter = static_cast<double>(total_ns) / static_cast<double>(iterations);

        std::cout << name << ": total_ns=" << total_ns << ", ns_per_iter=" << per_iter << ", sink=" << sink << '\n';
    }
}

int main()
{
    constexpr std::uint64_t iterations = 5'000'000;

    rpc::zone_address fixed_addr = *rpc::zone_address::create(rpc::zone_address::construction_args(
        rpc::zone_address::version_3, rpc::zone_address::address_type::local, 0, {}, 64, 0x1122334455667788ULL, 32, 0x87654321u, {}));
    rpc::zone_address fixed_peer = *rpc::zone_address::create(rpc::zone_address::construction_args(
        rpc::zone_address::version_3, rpc::zone_address::address_type::local, 0, {}, 64, 0x1122334455667788ULL, 32, 0x11111111u, {}));

    run_benchmark("get_subnet",
        iterations,
        [&](std::uint64_t count)
        {
            std::uint64_t total = 0;
            for (std::uint64_t i = 0; i < count; ++i)
            {
                total += fixed_addr.get_subnet();
            }
            return total;
        });

    run_benchmark("get_object_id",
        iterations,
        [&](std::uint64_t count)
        {
            std::uint64_t total = 0;
            for (std::uint64_t i = 0; i < count; ++i)
            {
                total += fixed_addr.get_object_id();
            }
            return total;
        });

    run_benchmark("same_zone",
        iterations,
        [&](std::uint64_t count)
        {
            std::uint64_t total = 0;
            for (std::uint64_t i = 0; i < count; ++i)
            {
                total += fixed_addr.same_zone(fixed_peer) ? 1u : 0u;
            }
            return total;
        });

    run_benchmark("hash_zone_address",
        iterations,
        [&](std::uint64_t count)
        {
            std::uint64_t total = 0;
            auto hasher = std::hash<rpc::zone_address>{};
            for (std::uint64_t i = 0; i < count; ++i)
            {
                total += static_cast<std::uint64_t>(hasher(fixed_addr));
            }
            return total;
        });

    return 0;
}
