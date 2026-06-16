/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <filesystem>
#include <string>

#ifndef CANOPY_STREAMING_BENCHMARK_CERT_DIR
#  error "CANOPY_STREAMING_BENCHMARK_CERT_DIR must point at the benchmark-local TLS certificate fixtures"
#endif

namespace stream_bench
{
    struct tls_fixture_cert_pair
    {
        std::string cert_path;
        std::string key_path;
        bool valid = false;

        tls_fixture_cert_pair()
        {
            const auto cert_dir = std::filesystem::path(CANOPY_STREAMING_BENCHMARK_CERT_DIR);
            cert_path = (cert_dir / "server.crt").string();
            key_path = (cert_dir / "server.key").string();
            valid = std::filesystem::exists(cert_path) && std::filesystem::exists(key_path);
        }

        tls_fixture_cert_pair(const tls_fixture_cert_pair&) = delete;
        auto operator=(const tls_fixture_cert_pair&) -> tls_fixture_cert_pair& = delete;
    };
}
