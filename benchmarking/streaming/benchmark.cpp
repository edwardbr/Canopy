/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Streaming Benchmark
 *   Measures raw stream throughput and round-trip latency for every stream type:
 *     - spsc          : SPSC queue pair (in-process, no network)
 *     - tcp           : TCP loopback
 *     - io_uring      : io_uring TCP loopback (Linux only)
 *     - tls+spsc      : TLS wrapping SPSC (measures pure TLS framing overhead)
 *     - ws+spsc       : WebSocket wrapping SPSC (measures pure WS framing overhead)
 *     - tls+ws+spsc   : WebSocket over TLS over SPSC (combined overhead)
 *
 *   Two scenarios per stream type, across a range of blob sizes:
 *     - unidirectional : sender pushes N blobs, receiver drains; reports send throughput
 *     - send_reply     : sender sends blob, waits for full echo; reports round-trip latency
 *
 *   No transport or service layer is exercised — raw streaming::stream send/receive only.
 *   Statistics: middle 80% of 1000 operations (drop first/last 10%), warmup 20 ops.
 *
 *   TLS note: a self-signed RSA-2048 certificate is generated at startup via OpenSSL.
 *   WebSocket note: server side uses streaming::websocket::stream (wslay server mode).
 *                   Client side uses ws_client_stream (wslay client mode, defined below).
 *
 *   Build:
 *     cmake --preset Debug_Coroutine -DCANOPY_BUILD_BENCHMARKING=ON
 *     cmake --build build_debug_coroutine --target streaming_benchmark
 *     ./build_debug_coroutine/output/streaming_benchmark
 */

#include <streaming/io_uring/acceptor.h>
#include <streaming/io_uring/stream.h>
#include <streaming/spsc_queue/stream.h>
#include <streaming/tcp/acceptor.h>
#include <streaming/tcp/stream.h>
#include <streaming/tls/stream.h>
#include <streaming/websocket/stream.h>

#include <rpc/rpc.h>
#include <coro/coro.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <wslay/wslay.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <iostream>
#include <mutex>
#include <numeric>
#include <queue>
#include <string>
#include <vector>

// ============================================================
// WebSocket client-mode stream
// Mirrors streaming::websocket::stream but uses
// wslay_event_context_client_init so outgoing frames are
// masked (RFC 6455 client requirement).
// ============================================================

namespace bench_helpers
{
    class ws_client_stream : public streaming::stream
    {
    public:
        explicit ws_client_stream(std::shared_ptr<::streaming::stream> underlying)
            : underlying_(std::move(underlying))
            , raw_recv_buffer_(4096, '\0')
        {
            wslay_event_callbacks cbs;
            std::memset(&cbs, 0, sizeof(cbs));
            cbs.recv_callback = recv_cb;
            cbs.send_callback = send_cb;
            cbs.on_msg_recv_callback = on_msg_cb;
            cbs.genmask_callback = genmask_cb;
            if (wslay_event_context_client_init(&ctx_, &cbs, this) != 0)
                throw std::runtime_error("ws_client_stream: wslay client init failed");
        }

        ~ws_client_stream() override
        {
            if (ctx_)
                wslay_event_context_free(ctx_);
        }

        ws_client_stream(const ws_client_stream&) = delete;
        auto operator=(const ws_client_stream&) -> ws_client_stream& = delete;

        auto receive(rpc::mutable_byte_span buf, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override
        {
            if (!decoded_.empty())
                co_return serve_decoded(buf);

            auto [status, span] = co_await underlying_->receive(
                rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()), timeout);
            if (status.is_closed())
            {
                closed_ = true;
                co_return {status, {}};
            }
            if (status.is_ok() && !span.empty())
            {
                raw_recv_pos_ = 0;
                raw_recv_size_ = span.size();
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    wslay_event_recv(ctx_);
                }
                if (!decoded_.empty())
                    co_return serve_decoded(buf);
            }
            co_return {status, {}};
        }

        auto send(rpc::byte_span buf) -> coro::task<coro::net::io_status> override
        {
            if (closed_)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            queue_message(std::vector<uint8_t>(buf.begin(), buf.end()));
            if (!co_await do_send())
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        bool is_closed() const override { return closed_; }
        void set_closed() override
        {
            closed_ = true;
            underlying_->set_closed();
        }
        auto get_peer_info() const -> streaming::peer_info override { return underlying_->get_peer_info(); }

    private:
        std::pair<coro::net::io_status, rpc::mutable_byte_span> serve_decoded(rpc::mutable_byte_span buf)
        {
            auto& msg = decoded_.front();
            size_t avail = msg.size() - msg_offset_;
            size_t n = std::min(avail, buf.size());
            std::memcpy(buf.data(), msg.data() + msg_offset_, n);
            msg_offset_ += n;
            if (msg_offset_ >= msg.size())
            {
                decoded_.pop();
                msg_offset_ = 0;
            }
            return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buf.subspan(0, n)};
        }

        void queue_message(std::vector<uint8_t> data)
        {
            std::lock_guard<std::mutex> lk(pending_mx_);
            pending_.push(std::move(data));
        }

        auto do_send() -> coro::task<bool>
        {
            {
                std::lock_guard<std::mutex> plk(pending_mx_);
                std::lock_guard<std::mutex> wlk(mx_);
                while (!pending_.empty())
                {
                    auto& d = pending_.front();
                    wslay_event_msg msg{};
                    msg.opcode = WSLAY_BINARY_FRAME;
                    msg.msg = d.data();
                    msg.msg_length = d.size();
                    wslay_event_queue_msg(ctx_, &msg);
                    pending_.pop();
                }
            }
            std::vector<uint8_t> to_write;
            {
                std::lock_guard<std::mutex> lk(mx_);
                wslay_event_send(ctx_);
                to_write = std::move(outgoing_raw_);
                outgoing_raw_ = {};
            }
            if (!to_write.empty())
            {
                auto st = co_await underlying_->send(
                    rpc::byte_span(reinterpret_cast<const char*>(to_write.data()), to_write.size()));
                if (!st.is_ok())
                    co_return false;
            }
            co_return true;
        }

        static int genmask_cb(wslay_event_context_ptr, uint8_t* buf, size_t len, void*)
        {
            // Generate random masking key bytes (required by RFC 6455 for client frames)
            for (size_t i = 0; i < len; ++i)
                buf[i] = static_cast<uint8_t>(std::rand() & 0xff);
            return 0;
        }

        static ssize_t send_cb(wslay_event_context_ptr, const uint8_t* data, size_t len, int, void* ud)
        {
            auto* self = static_cast<ws_client_stream*>(ud);
            self->outgoing_raw_.insert(self->outgoing_raw_.end(), data, data + len);
            return static_cast<ssize_t>(len);
        }

        static ssize_t recv_cb(wslay_event_context_ptr ctx, uint8_t* out, size_t len, int, void* ud)
        {
            auto* self = static_cast<ws_client_stream*>(ud);
            size_t avail = self->raw_recv_size_ - self->raw_recv_pos_;
            if (avail == 0)
            {
                wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
                return -1;
            }
            size_t n = std::min(len, avail);
            std::memcpy(out, self->raw_recv_buffer_.data() + self->raw_recv_pos_, n);
            self->raw_recv_pos_ += n;
            return static_cast<ssize_t>(n);
        }

        static void on_msg_cb(wslay_event_context_ptr, const wslay_event_on_msg_recv_arg* arg, void* ud)
        {
            auto* self = static_cast<ws_client_stream*>(ud);
            if (!wslay_is_ctrl_frame(arg->opcode) && arg->opcode == WSLAY_BINARY_FRAME)
                self->decoded_.emplace(arg->msg, arg->msg + arg->msg_length);
        }

        std::shared_ptr<::streaming::stream> underlying_;
        wslay_event_context_ptr ctx_{nullptr};
        mutable std::mutex mx_;
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0};
        size_t raw_recv_pos_{0};
        std::queue<std::vector<uint8_t>> decoded_;
        size_t msg_offset_{0};
        std::queue<std::vector<uint8_t>> pending_;
        std::mutex pending_mx_;
        std::vector<uint8_t> outgoing_raw_;
        bool closed_{false};
    };
} // namespace bench_helpers

// ============================================================
// TLS self-signed cert generation
// ============================================================

namespace bench_helpers
{
    struct temp_cert_pair
    {
        std::string cert_path;
        std::string key_path;
        bool valid = false;

        temp_cert_pair()
        {
            EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
            if (!kctx)
                return;
            EVP_PKEY* pkey = nullptr;
            if (EVP_PKEY_keygen_init(kctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0
                || EVP_PKEY_keygen(kctx, &pkey) <= 0)
            {
                EVP_PKEY_CTX_free(kctx);
                return;
            }
            EVP_PKEY_CTX_free(kctx);

            X509* x509 = X509_new();
            X509_set_version(x509, 2);
            ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
            X509_gmtime_adj(X509_getm_notBefore(x509), 0);
            X509_gmtime_adj(X509_getm_notAfter(x509), 86400L);
            X509_set_pubkey(x509, pkey);
            X509_NAME* name = X509_get_subject_name(x509);
            X509_NAME_add_entry_by_txt(
                name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("canopy-bench"), -1, -1, 0);
            X509_set_issuer_name(x509, name);
            X509_sign(x509, pkey, EVP_sha256());

            char ctmpl[] = "/tmp/canopy_bench_cert_XXXXXX.pem";
            char ktmpl[] = "/tmp/canopy_bench_key_XXXXXX.pem";
            int cfd = mkstemps(ctmpl, 4);
            int kfd = mkstemps(ktmpl, 4);
            if (cfd < 0 || kfd < 0)
            {
                X509_free(x509);
                EVP_PKEY_free(pkey);
                if (cfd >= 0)
                    close(cfd);
                if (kfd >= 0)
                    close(kfd);
                return;
            }
            FILE* cf = fdopen(cfd, "w");
            FILE* kf = fdopen(kfd, "w");
            PEM_write_X509(cf, x509);
            PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
            fclose(cf);
            fclose(kf);

            X509_free(x509);
            EVP_PKEY_free(pkey);

            cert_path = ctmpl;
            key_path = ktmpl;
            valid = true;
        }

        ~temp_cert_pair()
        {
            if (valid)
            {
                std::remove(cert_path.c_str());
                std::remove(key_path.c_str());
            }
        }

        temp_cert_pair(const temp_cert_pair&) = delete;
        auto operator=(const temp_cert_pair&) -> temp_cert_pair& = delete;
    };
} // namespace bench_helpers

// ============================================================
// Statistics (nanoseconds for unidirectional, microseconds for
// round-trip — same trimmed-80% approach as other benchmarks)
// ============================================================

namespace stream_bench
{
    using clock_type = std::chrono::steady_clock;

    constexpr size_t call_count = 1000;
    constexpr size_t warmup_count = 20;
    constexpr size_t trim_each_side = call_count / 10;

    struct bench_stats
    {
        double avg = 0.0;
        double min = 0.0;
        double max = 0.0;
        double p50 = 0.0;
        double p90 = 0.0;
        double p95 = 0.0;
        size_t blob_size = 0;
        bool valid = false;
    };

    bench_stats compute_stats(std::vector<int64_t> samples, size_t blob_size)
    {
        bench_stats s{};
        s.blob_size = blob_size;
        if (samples.size() < trim_each_side * 2)
            return s;

        std::sort(samples.begin(), samples.end());
        const size_t b = trim_each_side;
        const size_t e = samples.size() - trim_each_side;
        std::vector<int64_t> mid(samples.begin() + static_cast<long>(b), samples.begin() + static_cast<long>(e));
        const size_t n = mid.size();
        if (n == 0)
            return s;

        const auto sum = std::accumulate(mid.begin(), mid.end(), int64_t{0});
        s.avg = static_cast<double>(sum) / static_cast<double>(n);
        s.min = static_cast<double>(mid.front());
        s.max = static_cast<double>(mid.back());
        s.p50 = static_cast<double>(mid[(n * 50) / 100]);
        s.p90 = static_cast<double>(mid[(n * 90) / 100]);
        s.p95 = static_cast<double>(mid[(n * 95) / 100]);
        s.valid = true;
        return s;
    }

    static const std::vector<size_t> blob_sizes = {
        64,      // 64 B
        256,     // 256 B
        1024,    // 1 KB
        4096,    // 4 KB
        16384,   // 16 KB
        65536,   // 64 KB
        131072,  // 128 KB
        262144,  // 256 KB
        524288,  // 512 KB
        1048576, // 1 MB
    };

    // ---- Scheduler factory ----

    std::shared_ptr<coro::scheduler> make_scheduler()
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = 2},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }

    // ---- SPSC pair ----

    struct spsc_pipe
    {
        streaming::spsc_queue::queue_type q_a_to_b;
        streaming::spsc_queue::queue_type q_b_to_a;

        std::shared_ptr<streaming::stream> side_a(std::shared_ptr<coro::scheduler> sched)
        {
            return std::make_shared<streaming::spsc_queue::stream>(&q_a_to_b, &q_b_to_a, std::move(sched));
        }

        std::shared_ptr<streaming::stream> side_b(std::shared_ptr<coro::scheduler> sched)
        {
            return std::make_shared<streaming::spsc_queue::stream>(&q_b_to_a, &q_a_to_b, std::move(sched));
        }
    };

    // ============================================================
    // Generic benchmark coroutines
    // ============================================================

    // Unidirectional sender: sends call_count blobs, records per-send timing.
    coro::task<bench_stats> run_unidirectional_sender(
        std::shared_ptr<streaming::stream> stm, const std::vector<uint8_t>& payload, std::atomic<bool>& stop)
    {
        std::vector<int64_t> samples;
        samples.reserve(call_count + warmup_count);

        for (size_t i = 0; i < warmup_count + call_count; ++i)
        {
            const auto t0 = clock_type::now();
            auto status = co_await stm->send(rpc::byte_span(payload));
            const auto t1 = clock_type::now();

            if (!status.is_ok())
                break;

            if (i >= warmup_count)
                samples.push_back(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }

        stop.store(true, std::memory_order_release);
        co_return compute_stats(std::move(samples), payload.size());
    }

    // Drains the stream until stop is set by the sender.
    coro::task<void> run_drain(std::shared_ptr<streaming::stream> stm, const std::atomic<bool>& stop)
    {
        std::vector<uint8_t> buf(1 << 20);
        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stm->receive(rpc::mutable_byte_span(buf), std::chrono::milliseconds{10});
            if (status.is_closed())
                break;
        }
    }

    // Send-reply sender: one send + full receive echo per measurement.
    coro::task<bench_stats> run_send_reply(
        std::shared_ptr<streaming::stream> stm, const std::vector<uint8_t>& payload, std::atomic<bool>& stop)
    {
        std::vector<int64_t> samples;
        samples.reserve(call_count + warmup_count);
        std::vector<uint8_t> recv_buf(payload.size() + 256);

        for (size_t i = 0; i < warmup_count + call_count; ++i)
        {
            const auto t0 = clock_type::now();

            auto send_st = co_await stm->send(rpc::byte_span(payload));
            if (!send_st.is_ok())
                break;

            // Accumulate until we have all bytes echoed back.
            size_t received = 0;
            bool failed = false;
            while (received < payload.size())
            {
                auto [status, span] = co_await stm->receive(
                    rpc::mutable_byte_span(recv_buf.data() + received, recv_buf.size() - received),
                    std::chrono::milliseconds{1000});
                if (status.is_closed())
                {
                    failed = true;
                    break;
                }
                received += span.size();
            }
            if (failed)
                break;

            const auto t1 = clock_type::now();

            if (i >= warmup_count)
                samples.push_back(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
        }

        stop.store(true, std::memory_order_release);
        co_return compute_stats(std::move(samples), payload.size());
    }

    // Echo server: receives and echoes back until stop is set.
    coro::task<void> run_echo(std::shared_ptr<streaming::stream> stm, const std::atomic<bool>& stop)
    {
        std::vector<uint8_t> buf(1 << 20);
        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stm->receive(rpc::mutable_byte_span(buf), std::chrono::milliseconds{10});
            if (status.is_closed())
                break;
            if (status.is_ok() && !span.empty())
                co_await stm->send(rpc::byte_span(span));
        }
    }

    // ============================================================
    // Reporting
    // ============================================================

    void print_unidirectional_header()
    {
        fmt::print("\n=== Unidirectional (send throughput) — {} sends, middle 80%, warmup {}\n", call_count, warmup_count);
        fmt::print("Units: send time in ns\n");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
        fmt::print("{:<27} | {:>10} | {:>13} | {:>10} | {:>10} | {:>10} | {:>10} | {:>11}\n",
            "stream_type",
            "blob_bytes",
            "throughput MB/s",
            "avg_ns",
            "p50_ns",
            "p90_ns",
            "p95_ns",
            "max_ns");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
    }

    void print_unidirectional_row(const char* type, const bench_stats& s)
    {
        if (!s.valid)
        {
            fmt::print("{:<27} | {:>10} | error\n", type, s.blob_size);
            return;
        }
        // throughput: blob_size bytes per send / avg_ns per send = bytes/ns = GB/s → * 1000 = MB/s
        const double throughput = (s.avg > 0.0) ? (static_cast<double>(s.blob_size) / s.avg * 1000.0) : 0.0;
        fmt::print("{:<27} | {:>10} | {:>13.2f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:>11.1f}\n",
            type,
            s.blob_size,
            throughput,
            s.avg,
            s.p50,
            s.p90,
            s.p95,
            s.max);
    }

    void print_send_reply_header()
    {
        fmt::print(
            "\n=== Send-Reply (round-trip latency) — {} round-trips, middle 80%, warmup {}\n", call_count, warmup_count);
        fmt::print("Units: latency in µs\n");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
        fmt::print("{:<27} | {:>10} | {:>13} | {:>10} | {:>10} | {:>10} | {:>10} | {:>11}\n",
            "stream_type",
            "blob_bytes",
            "throughput MB/s",
            "avg_µs",
            "p50_µs",
            "p90_µs",
            "p95_µs",
            "max_µs");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
    }

    void print_send_reply_row(const char* type, const bench_stats& s)
    {
        if (!s.valid)
        {
            fmt::print("{:<27} | {:>10} | error\n", type, s.blob_size);
            return;
        }
        // throughput: blob_size bytes / avg_µs = bytes/µs = MB/s (SI)
        const double throughput = (s.avg > 0.0) ? (static_cast<double>(s.blob_size) / s.avg) : 0.0;
        fmt::print("{:<27} | {:>10} | {:>13.2f} | {:>10.2f} | {:>10.2f} | {:>10.2f} | {:>10.2f} | {:>11.2f}\n",
            type,
            s.blob_size,
            throughput,
            s.avg,
            s.p50,
            s.p90,
            s.p95,
            s.max);
    }

    // ============================================================
    // SPSC-based benchmark helper
    // Both stream sides are created synchronously from a shared
    // spsc_pipe; no network setup needed.
    // ============================================================

    void run_spsc_based_bench(const char* name,
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        std::shared_ptr<coro::scheduler> sched_a,
        std::shared_ptr<coro::scheduler> sched_b,
        size_t blob_size,
        bench_stats& out_unidirectional,
        bench_stats& out_send_reply)
    {
        const std::vector<uint8_t> payload(blob_size, 0xAB);

        // --- Unidirectional ---
        {
            std::atomic<bool> stop{false};
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            coro::sync_wait(coro::when_all([&]() -> coro::task<void>
                { out_unidirectional = co_await run_unidirectional_sender(side_a, payload, stop); }(),
                run_drain(side_b, stop)));
        }

        // --- Send-Reply ---
        {
            std::atomic<bool> stop{false};
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            coro::sync_wait(coro::when_all([&]() -> coro::task<void>
                { out_send_reply = co_await run_send_reply(side_a, payload, stop); }(),
                run_echo(side_b, stop)));
        }

        (void)name;
        (void)sched_a;
        (void)sched_b;
    }

    // ============================================================
    // TCP / io_uring benchmark helpers
    // Server task accepts one connection, client task connects.
    // ============================================================

    void run_tcp_bench(const char* name, size_t blob_size, uint16_t port, bench_stats& out_uni, bench_stats& out_reply)
    {
        const std::vector<uint8_t> payload(blob_size, 0xAB);
        const coro::net::socket_address endpoint{"127.0.0.1", port};

        auto sched_server = make_scheduler();
        auto sched_client = make_scheduler();

        // --- Unidirectional ---
        {
            std::atomic<bool> stop{false};
            rpc::event server_ready;
            coro::sync_wait(coro::when_all(
                // Server: accept, drain
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    auto server = std::make_shared<coro::net::tcp::server>(sched_server, endpoint);
                    server_ready.set();
                    auto accepted = co_await server->accept(std::chrono::milliseconds{5000});
                    if (!accepted)
                        co_return;
                    auto stm = std::make_shared<streaming::tcp::stream>(std::move(*accepted), sched_server);
                    co_await run_drain(stm, stop);
                }(),
                // Client: connect, send
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    co_await server_ready.wait();
                    coro::net::tcp::client client(sched_client, endpoint);
                    if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                        co_return;
                    auto stm = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                    out_uni = co_await run_unidirectional_sender(stm, payload, stop);
                }()));
        }

        // --- Send-Reply ---
        {
            std::atomic<bool> stop{false};
            rpc::event server_ready;
            coro::sync_wait(coro::when_all(
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    auto server = std::make_shared<coro::net::tcp::server>(sched_server, endpoint);
                    server_ready.set();
                    auto accepted = co_await server->accept(std::chrono::milliseconds{5000});
                    if (!accepted)
                        co_return;
                    auto stm = std::make_shared<streaming::tcp::stream>(std::move(*accepted), sched_server);
                    co_await run_echo(stm, stop);
                }(),
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    co_await server_ready.wait();
                    coro::net::tcp::client client(sched_client, endpoint);
                    if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                        co_return;
                    auto stm = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                    out_reply = co_await run_send_reply(stm, payload, stop);
                }()));
        }

        sched_server->shutdown();
        sched_client->shutdown();
        (void)name;
    }

#ifdef __linux__
    void run_io_uring_bench(const char* name, size_t blob_size, uint16_t port, bench_stats& out_uni, bench_stats& out_reply)
    {
        const std::vector<uint8_t> payload(blob_size, 0xAB);
        canopy::network_config::ip_address addr{};
        addr[0] = 127;
        addr[1] = 0;
        addr[2] = 0;
        addr[3] = 1;

        auto sched_server = make_scheduler();
        auto sched_client = make_scheduler();

        auto run_bench = [&](std::atomic<bool>& stop, rpc::event& server_ready, auto server_fn, auto client_fn)
        {
            (void)stop;
            auto acc = std::make_shared<streaming::io_uring::acceptor>(addr, port);
            acc->init(sched_server);

            coro::sync_wait(coro::when_all(
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    server_ready.set();
                    auto maybe = co_await acc->accept();
                    if (!maybe)
                        co_return;
                    co_await server_fn(*maybe);
                    acc->stop();
                }(),
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    co_await server_ready.wait();
                    coro::net::tcp::client client(sched_client, coro::net::socket_address{"127.0.0.1", port});
                    if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                        co_return;
                    auto stm = std::make_shared<streaming::io_uring::stream>(std::move(client), sched_client);
                    co_await client_fn(stm);
                }()));
        };

        {
            std::atomic<bool> stop{false};
            rpc::event ready;
            run_bench(
                stop,
                ready,
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void> { co_await run_drain(stm, stop); },
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void>
                { out_uni = co_await run_unidirectional_sender(stm, payload, stop); });
        }

        {
            std::atomic<bool> stop{false};
            rpc::event ready;
            run_bench(
                stop,
                ready,
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void> { co_await run_echo(stm, stop); },
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void>
                { out_reply = co_await run_send_reply(stm, payload, stop); });
        }

        sched_server->shutdown();
        sched_client->shutdown();
        (void)name;
    }
#endif

    // ============================================================
    // Per-blob-size runner: invokes bench, prints both rows
    // ============================================================

    template<typename BenchFn>
    void run_all_sizes(const char* name,
        BenchFn&& bench_fn,
        std::vector<bench_stats>& unidirectional_results,
        std::vector<bench_stats>& send_reply_results)
    {
        (void)name;
        for (size_t blob_size : blob_sizes)
        {
            bench_stats uni{};
            bench_stats reply{};
            bench_fn(blob_size, uni, reply);
            unidirectional_results.push_back(uni);
            send_reply_results.push_back(reply);
        }
    }

} // namespace stream_bench

// ============================================================
// Main
// ============================================================

int main()
{
    using namespace stream_bench;
    using namespace bench_helpers;

    std::cout << "RPC++ Streaming Benchmark\n";
    std::cout << "=========================\n\n";

    // Generate TLS cert once for all TLS benchmarks.
    temp_cert_pair tls_cert;
    if (!tls_cert.valid)
    {
        std::cerr << "WARNING: TLS cert generation failed — TLS benchmarks will be skipped\n";
    }

    // ---- Collect results per stream type ----

    std::vector<std::pair<std::string, std::pair<std::vector<bench_stats>, std::vector<bench_stats>>>> all_results;

    auto collect = [&](const char* name, auto bench_fn)
    {
        RPC_INFO("Doing: {}", name);
        std::vector<bench_stats> uni;
        std::vector<bench_stats> reply;
        run_all_sizes(name, bench_fn, uni, reply);
        all_results.push_back({name, {std::move(uni), std::move(reply)}});
    };

    // --- spsc ---
    collect("spsc",
        [](size_t blob_size, bench_stats& uni, bench_stats& reply)
        {
            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            auto side_a = pipe->side_a(sched_a);
            auto side_b = pipe->side_b(sched_b);
            run_spsc_based_bench("spsc", side_a, side_b, sched_a, sched_b, blob_size, uni, reply);
            sched_a->shutdown();
            sched_b->shutdown();
        });

    // --- tcp ---
    {
        uint16_t port = 19200;
        collect("tcp",
            [&port](size_t blob_size, bench_stats& uni, bench_stats& reply)
            {
                run_tcp_bench("tcp", blob_size, port, uni, reply);
                port += 2;
            });
    }

#ifdef __linux__
    // --- io_uring ---
    {
        uint16_t port = 19400;
        collect("io_uring",
            [&port](size_t blob_size, bench_stats& uni, bench_stats& reply)
            {
                run_io_uring_bench("io_uring", blob_size, port, uni, reply);
                port += 2;
            });
    }
#endif

    // --- tls+spsc ---
    if (tls_cert.valid)
    {
        collect("tls+spsc",
            [&tls_cert](size_t blob_size, bench_stats& uni, bench_stats& reply)
            {
                auto sched_a = make_scheduler();
                auto sched_b = make_scheduler();
                auto pipe = std::make_unique<spsc_pipe>();
                auto raw_a = pipe->side_a(sched_a);
                auto raw_b = pipe->side_b(sched_b);

                auto server_ctx = std::make_shared<streaming::tls::context>(tls_cert.cert_path, tls_cert.key_path);
                auto client_ctx = std::make_shared<streaming::tls::client_context>(false);
                if (!server_ctx->is_valid() || !client_ctx->is_valid())
                    return;

                std::shared_ptr<streaming::stream> tls_a;
                std::shared_ptr<streaming::stream> tls_b;
                coro::sync_wait(coro::when_all(
                    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                    [&]() -> coro::task<void>
                    {
                        auto s = std::make_shared<streaming::tls::stream>(raw_a, server_ctx);
                        if (co_await s->handshake())
                            tls_a = s;
                    }(),
                    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                    [&]() -> coro::task<void>
                    {
                        auto s = std::make_shared<streaming::tls::stream>(raw_b, client_ctx);
                        if (co_await s->client_handshake())
                            tls_b = s;
                    }()));

                if (!tls_a || !tls_b)
                    return;

                run_spsc_based_bench("tls+spsc", tls_a, tls_b, sched_a, sched_b, blob_size, uni, reply);
                sched_a->shutdown();
                sched_b->shutdown();
            });
    }

    // --- ws+spsc ---
    collect("ws+spsc",
        [](size_t blob_size, bench_stats& uni, bench_stats& reply)
        {
            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            // side_a is the WS server, side_b is the WS client
            auto ws_server = std::make_shared<streaming::websocket::stream>(pipe->side_a(sched_a));
            auto ws_client = std::make_shared<ws_client_stream>(pipe->side_b(sched_b));
            run_spsc_based_bench("ws+spsc", ws_server, ws_client, sched_a, sched_b, blob_size, uni, reply);
            sched_a->shutdown();
            sched_b->shutdown();
        });

    // --- tls+ws+spsc ---
    if (tls_cert.valid)
    {
        collect("tls+ws+spsc",
            [&tls_cert](size_t blob_size, bench_stats& uni, bench_stats& reply)
            {
                auto sched_a = make_scheduler();
                auto sched_b = make_scheduler();
                auto pipe = std::make_unique<spsc_pipe>();
                auto raw_a = pipe->side_a(sched_a);
                auto raw_b = pipe->side_b(sched_b);

                auto server_ctx = std::make_shared<streaming::tls::context>(tls_cert.cert_path, tls_cert.key_path);
                auto client_ctx = std::make_shared<streaming::tls::client_context>(false);
                if (!server_ctx->is_valid() || !client_ctx->is_valid())
                    return;

                std::shared_ptr<streaming::stream> tls_a;
                std::shared_ptr<streaming::stream> tls_b;
                coro::sync_wait(coro::when_all(
                    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                    [&]() -> coro::task<void>
                    {
                        auto s = std::make_shared<streaming::tls::stream>(raw_a, server_ctx);
                        if (co_await s->handshake())
                            tls_a = s;
                    }(),
                    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                    [&]() -> coro::task<void>
                    {
                        auto s = std::make_shared<streaming::tls::stream>(raw_b, client_ctx);
                        if (co_await s->client_handshake())
                            tls_b = s;
                    }()));

                if (!tls_a || !tls_b)
                    return;

                auto ws_server = std::make_shared<streaming::websocket::stream>(tls_a);
                auto ws_client = std::make_shared<ws_client_stream>(tls_b);
                run_spsc_based_bench("tls+ws+spsc", ws_server, ws_client, sched_a, sched_b, blob_size, uni, reply);
                sched_a->shutdown();
                sched_b->shutdown();
            });
    }

    // ---- Print results ----

    print_unidirectional_header();
    for (const auto& [name, results] : all_results)
    {
        const auto& uni = results.first;
        for (const auto& s : uni)
            print_unidirectional_row(name.c_str(), s);
    }

    print_send_reply_header();
    for (const auto& [name, results] : all_results)
    {
        const auto& reply = results.second;
        for (const auto& s : reply)
            print_send_reply_row(name.c_str(), s);
    }

    fmt::print("\nDone.\n");
    return 0;
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string msg(str, sz);
    switch (level)
    {
    case 0:
        std::cout << "[TRACE] " << msg << '\n';
        break;
    case 1:
        std::cout << "[DEBUG] " << msg << '\n';
        break;
    case 2:
        std::cout << "[INFO] " << msg << '\n';
        break;
    case 3:
        std::cout << "[WARN] " << msg << '\n';
        break;
    case 4:
        std::cout << "[ERROR] " << msg << '\n';
        break;
    case 5:
        std::cout << "[CRIT] " << msg << '\n';
        break;
    default:
        std::cout << "[LOG " << level << "] " << msg << '\n';
        break;
    }
}
