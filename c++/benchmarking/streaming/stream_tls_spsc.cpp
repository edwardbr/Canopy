/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <streaming/spsc_queue/stream.h>
#include <streaming/tls/stream.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdio>
#include <unistd.h>

namespace stream_bench
{
    namespace
    {
        struct temp_cert_pair
        {
            std::string cert_path;
            std::string key_path;
            bool valid = false;

            temp_cert_pair()
            {
                EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
                if (!key_context)
                    return;

                EVP_PKEY* private_key = nullptr;
                if (EVP_PKEY_keygen_init(key_context) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) <= 0
                    || EVP_PKEY_keygen(key_context, &private_key) <= 0)
                {
                    EVP_PKEY_CTX_free(key_context);
                    return;
                }
                EVP_PKEY_CTX_free(key_context);

                X509* certificate = X509_new();
                X509_set_version(certificate, 2);
                ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
                X509_gmtime_adj(X509_getm_notBefore(certificate), 0);
                X509_gmtime_adj(X509_getm_notAfter(certificate), 86400L);
                X509_set_pubkey(certificate, private_key);
                X509_NAME* name = X509_get_subject_name(certificate);
                X509_NAME_add_entry_by_txt(
                    name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("canopy-bench"), -1, -1, 0);
                X509_set_issuer_name(certificate, name);
                X509_sign(certificate, private_key, EVP_sha256());

                char cert_template[] = "/tmp/canopy_bench_cert_XXXXXX.pem";
                char key_template[] = "/tmp/canopy_bench_key_XXXXXX.pem";
                int cert_fd = mkstemps(cert_template, 4);
                int key_fd = mkstemps(key_template, 4);
                if (cert_fd < 0 || key_fd < 0)
                {
                    X509_free(certificate);
                    EVP_PKEY_free(private_key);
                    if (cert_fd >= 0)
                        close(cert_fd);
                    if (key_fd >= 0)
                        close(key_fd);
                    return;
                }

                FILE* cert_file = fdopen(cert_fd, "w");
                FILE* key_file = fdopen(key_fd, "w");
                PEM_write_X509(cert_file, certificate);
                PEM_write_PrivateKey(key_file, private_key, nullptr, nullptr, 0, nullptr, nullptr);
                fclose(cert_file);
                fclose(key_file);

                X509_free(certificate);
                EVP_PKEY_free(private_key);

                cert_path = cert_template;
                key_path = key_template;
                valid = true;
            }

            ~temp_cert_pair()
            {
                if (!valid)
                    return;

                std::remove(cert_path.c_str());
                std::remove(key_path.c_str());
            }

            temp_cert_pair(const temp_cert_pair&) = delete;
            auto operator=(const temp_cert_pair&) -> temp_cert_pair& = delete;
        };

        struct spsc_pipe
        {
            streaming::spsc_queue::queue_type q_a_to_b;
            streaming::spsc_queue::queue_type q_b_to_a;

            std::shared_ptr<streaming::stream> side_a(std::shared_ptr<coro::scheduler> scheduler)
            {
                return std::make_shared<streaming::spsc_queue::stream>(&q_a_to_b, &q_b_to_a, std::move(scheduler));
            }

            std::shared_ptr<streaming::stream> side_b(std::shared_ptr<coro::scheduler> scheduler)
            {
                return std::make_shared<streaming::spsc_queue::stream>(&q_b_to_a, &q_a_to_b, std::move(scheduler));
            }
        };

        bool make_tls_pair(
            const temp_cert_pair& cert,
            const std::shared_ptr<streaming::stream>& raw_a,
            const std::shared_ptr<streaming::stream>& raw_b,
            const std::shared_ptr<coro::scheduler>& scheduler_a,
            const std::shared_ptr<coro::scheduler>& scheduler_b,
            std::shared_ptr<streaming::stream>& tls_a,
            std::shared_ptr<streaming::stream>& tls_b)
        {
            auto server_context = std::make_shared<streaming::tls::context>(cert.cert_path, cert.key_path);
            auto client_context = std::make_shared<streaming::tls::client_context>(false);
            if (!server_context->is_valid() || !client_context->is_valid())
                return false;

            coro::sync_wait(
                coro::when_all(
                    scheduler_a->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::tls::stream>(raw_a, server_context);
                            if (co_await stream->handshake())
                                tls_a = stream;
                        }()),
                    scheduler_b->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::tls::stream>(raw_b, client_context);
                            if (co_await stream->client_handshake())
                                tls_b = stream;
                        }())));

            return tls_a && tls_b;
        }

        void run_standard_tls_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            temp_cert_pair cert;
            if (!cert.valid)
                return;

            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            std::shared_ptr<streaming::stream> tls_a;
            std::shared_ptr<streaming::stream> tls_b;
            if (make_tls_pair(cert, pipe->side_a(sched_a), pipe->side_b(sched_b), sched_a, sched_b, tls_a, tls_b))
                run_paired_stream_bench(tls_a, tls_b, cfg, wd, blob_size, unidirectional, send_reply);

            sched_a->shutdown();
            sched_b->shutdown();
        }

        void run_stress_tls_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            temp_cert_pair cert;
            if (!cert.valid)
                return;

            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            std::shared_ptr<streaming::stream> tls_a;
            std::shared_ptr<streaming::stream> tls_b;
            if (make_tls_pair(cert, pipe->side_a(sched_a), pipe->side_b(sched_b), sched_a, sched_b, tls_a, tls_b))
                run_paired_stream_stress_bench(tls_a, tls_b, cfg, wd, blob_size, send, recv);

            sched_a->shutdown();
            sched_b->shutdown();
        }
    } // namespace

    void add_tls_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "tls+spsc"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tls+spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_tls_spsc(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"tls+spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tls_spsc(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
