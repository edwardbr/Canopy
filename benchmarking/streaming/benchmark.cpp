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
 *   Scenarios:
 *     - unidirectional : sender pushes N blobs, receiver drains; reports send throughput
 *     - send_reply     : sender sends blob, waits for full echo; reports round-trip latency
 *     - stress         : sender saturates stream for D seconds; watchdog aborts on hang
 *
 *   No transport or service layer is exercised — raw streaming::stream send/receive only.
 *   Statistics: middle 80% of N operations (drop first/last 10%), warmup 20 ops.
 *
 *   TLS note: a self-signed RSA-2048 certificate is generated at startup via OpenSSL.
 *   WebSocket note: server side uses streaming::websocket::stream (wslay server mode).
 *                   Client side uses ws_client_stream (wslay client mode, defined below).
 *
 *   Usage:
 *     ./streaming_benchmark [options]
 *
 *     --stream <name>       Run only this stream type (repeat for multiple; default: all)
 *                           Valid: spsc, tcp, io_uring, tls+spsc, ws+spsc, tls+ws+spsc
 *     --scenario <s>        unidirectional | send_reply | stress | all  (default: all)
 *     --count <N>           Measurement iterations per blob size (default: 1000)
 *     --warmup <N>          Warmup iterations (default: 20)
 *     --blob-size <bytes>   Single blob size instead of the full sweep
 *     --timeout-ms <ms>     Per-receive timeout for send_reply scenario (default: 1000)
 *     --duration-s <N>      Stress test duration in seconds (default: 30)
 *     --watchdog-ms <ms>    Abort if no progress for this long; 0=disabled (default: 10000)
 *
 *   Examples:
 *     ./streaming_benchmark --stream io_uring --scenario stress --duration-s 60
 *     ./streaming_benchmark --stream tcp --stream spsc --scenario unidirectional
 *     ./streaming_benchmark --stream io_uring --scenario all --watchdog-ms 5000
 *
 *   Build:
 *     cmake --preset Debug_Coroutine -DCANOPY_BUILD_BENCHMARKING=ON
 *     cmake --build build_debug_coroutine --target streaming_benchmark
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
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// WebSocket client-mode stream
// Mirrors streaming::websocket::stream but uses
// wslay_event_context_client_init so outgoing frames are
// masked (RFC 6455 client requirement).
// ============================================================

namespace bench_helpers
{
    namespace
    {
        constexpr size_t websocket_io_chunk_size = 8192;

        auto remaining_timeout(std::chrono::steady_clock::time_point deadline) -> std::chrono::milliseconds
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return std::chrono::milliseconds{0};

            return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        }
    } // namespace

    class ws_client_stream : public streaming::stream
    {
    public:
        explicit ws_client_stream(std::shared_ptr<::streaming::stream> underlying)
            : underlying_(std::move(underlying))
            , raw_recv_buffer_(websocket_io_chunk_size, '\0')
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

            auto deadline = std::chrono::steady_clock::now() + timeout;
            bool single_attempt = timeout <= std::chrono::milliseconds{0};

            while (true)
            {
                if (!co_await do_send())
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};

                auto [status, span] = co_await underlying_->receive(
                    rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()),
                    single_attempt ? std::chrono::milliseconds{0} : remaining_timeout(deadline));
                if (status.is_closed())
                {
                    closed_ = true;
                    co_return {status, {}};
                }
                if (status.is_ok() && !span.empty())
                {
                    raw_recv_pos_ = 0;
                    raw_recv_size_ = span.size();
                    wslay_event_recv(ctx_);
                    if (!co_await do_send())
                        co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
                    if (!decoded_.empty())
                        co_return serve_decoded(buf);
                }
                else if (status.is_timeout() || span.empty())
                {
                    if (single_attempt || std::chrono::steady_clock::now() >= deadline)
                        co_return {status, {}};
                }
                else
                {
                    co_return {status, {}};
                }

                if (single_attempt)
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
            }
        }

        auto send(rpc::byte_span buf) -> coro::task<coro::net::io_status> override
        {
            if (closed_)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            // wslay copies the payload internally so we can pass the span directly.
            wslay_event_msg msg{};
            msg.opcode = WSLAY_BINARY_FRAME;
            msg.msg = reinterpret_cast<const uint8_t*>(buf.data());
            msg.msg_length = buf.size();
            if (wslay_event_queue_msg(ctx_, &msg) != 0)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            if (!co_await do_send())
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        bool is_closed() const override { return closed_; }
        auto set_closed() -> coro::task<void> override
        {
            closed_ = true;
            if (underlying_)
                co_await underlying_->set_closed();
            co_return;
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

        auto do_send() -> coro::task<bool>
        {
            while (wslay_event_want_write(ctx_))
            {
                outgoing_raw_.clear();
                wslay_event_send(ctx_);
                size_t offset = 0;
                while (offset < outgoing_raw_.size())
                {
                    size_t chunk_size = std::min(websocket_io_chunk_size, outgoing_raw_.size() - offset);
                    auto st = co_await underlying_->send(
                        rpc::byte_span(reinterpret_cast<const char*>(outgoing_raw_.data() + offset), chunk_size));
                    if (!st.is_ok())
                        co_return false;
                    offset += chunk_size;
                }
                outgoing_raw_.clear();
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
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0};
        size_t raw_recv_pos_{0};
        std::queue<std::vector<uint8_t>> decoded_;
        size_t msg_offset_{0};
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
// bench_config and argument parsing
// ============================================================

namespace stream_bench
{
    struct bench_config
    {
        std::set<std::string> streams; // empty = all
        bool run_unidirectional = true;
        bool run_send_reply = true;
        bool run_stress = false;
        size_t count = 1000;
        size_t warmup = 20;
        std::optional<size_t> blob_size_override;
        std::chrono::milliseconds recv_timeout{1000};
        std::chrono::seconds stress_duration{30};
        std::chrono::milliseconds watchdog_timeout{10000};
    };

    static void print_usage(const char* prog)
    {
        fmt::print("Usage: {} [options]\n"
                   "\n"
                   "  --stream <name>       Stream type to benchmark (repeat for multiple; default: all)\n"
                   "                        Valid: spsc, tcp, io_uring, tls+spsc, ws+spsc, tls+ws+spsc\n"
                   "  --scenario <s>        unidirectional | send_reply | stress | all  (default: all)\n"
                   "  --count <N>           Measurement iterations (default: 1000)\n"
                   "  --warmup <N>          Warmup iterations (default: 20)\n"
                   "  --blob-size <bytes>   Single blob size instead of the full sweep\n"
                   "  --timeout-ms <ms>     Per-receive timeout for send_reply in ms (default: 1000)\n"
                   "  --duration-s <N>      Stress test duration in seconds (default: 30)\n"
                   "  --watchdog-ms <ms>    Abort if no progress for this long; 0=disabled (default: 10000)\n"
                   "\n",
            prog);
    }

    static bench_config parse_args(int argc, char** argv)
    {
        bench_config cfg;
        bool scenario_set = false;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                print_usage(argv[0]);
                std::exit(0);
            }
            else if (arg == "--stream" && i + 1 < argc)
            {
                cfg.streams.insert(argv[++i]);
            }
            else if (arg == "--scenario" && i + 1 < argc)
            {
                std::string s = argv[++i];
                if (!scenario_set)
                {
                    cfg.run_unidirectional = false;
                    cfg.run_send_reply = false;
                    cfg.run_stress = false;
                    scenario_set = true;
                }
                if (s == "unidirectional")
                    cfg.run_unidirectional = true;
                else if (s == "send_reply")
                    cfg.run_send_reply = true;
                else if (s == "stress")
                    cfg.run_stress = true;
                else if (s == "all")
                {
                    cfg.run_unidirectional = true;
                    cfg.run_send_reply = true;
                    cfg.run_stress = true;
                }
                else
                {
                    fmt::print(stderr, "Unknown scenario '{}'; valid: unidirectional, send_reply, stress, all\n", s);
                    std::exit(1);
                }
            }
            else if (arg == "--count" && i + 1 < argc)
            {
                cfg.count = static_cast<size_t>(std::stoull(argv[++i]));
            }
            else if (arg == "--warmup" && i + 1 < argc)
            {
                cfg.warmup = static_cast<size_t>(std::stoull(argv[++i]));
            }
            else if (arg == "--blob-size" && i + 1 < argc)
            {
                cfg.blob_size_override = static_cast<size_t>(std::stoull(argv[++i]));
            }
            else if (arg == "--timeout-ms" && i + 1 < argc)
            {
                cfg.recv_timeout = std::chrono::milliseconds{std::stoull(argv[++i])};
            }
            else if (arg == "--duration-s" && i + 1 < argc)
            {
                cfg.stress_duration = std::chrono::seconds{std::stoull(argv[++i])};
            }
            else if (arg == "--watchdog-ms" && i + 1 < argc)
            {
                cfg.watchdog_timeout = std::chrono::milliseconds{std::stoull(argv[++i])};
            }
            else
            {
                fmt::print(stderr, "Unknown argument: {}\n", arg);
                print_usage(argv[0]);
                std::exit(1);
            }
        }
        return cfg;
    }
} // namespace stream_bench

// ============================================================
// Watchdog — aborts the process if no heartbeat within timeout
// ============================================================

namespace stream_bench
{
    class watchdog
    {
    public:
        explicit watchdog(std::chrono::milliseconds timeout)
            : timeout_(timeout)
            , last_ns_(now_ns())
            , stop_(false)
        {
            if (timeout_.count() > 0)
                thread_ = std::thread([this] { run(); });
        }

        ~watchdog()
        {
            stop_.store(true, std::memory_order_relaxed);
            if (thread_.joinable())
                thread_.join();
        }

        watchdog(const watchdog&) = delete;
        auto operator=(const watchdog&) -> watchdog& = delete;

        void heartbeat() { last_ns_.store(now_ns(), std::memory_order_relaxed); }

        void set_context(const std::string& ctx)
        {
            std::lock_guard<std::mutex> lk(ctx_mx_);
            context_ = ctx;
        }

    private:
        static int64_t now_ns() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

        void run()
        {
            while (!stop_.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
                if (stop_.load(std::memory_order_relaxed))
                    break;
                const int64_t elapsed_ms = (now_ns() - last_ns_.load(std::memory_order_relaxed)) / 1'000'000LL;
                if (elapsed_ms > timeout_.count())
                {
                    std::string ctx;
                    {
                        std::lock_guard<std::mutex> lk(ctx_mx_);
                        ctx = context_;
                    }
                    fmt::print(stderr,
                        "\n[WATCHDOG] No progress for {}ms (limit {}ms){} — aborting\n"
                        "           This indicates a permanent hang (likely a send-side deadlock).\n"
                        "           Hint: if the stream is legitimately slow, increase --watchdog-ms.\n",
                        elapsed_ms,
                        timeout_.count(),
                        ctx.empty() ? "" : fmt::format(" during '{}'", ctx));
                    std::fflush(stderr);
                    std::abort();
                }
            }
        }

        std::chrono::milliseconds timeout_;
        std::atomic<int64_t> last_ns_;
        std::atomic<bool> stop_;
        std::thread thread_;
        std::mutex ctx_mx_;
        std::string context_;
    };
} // namespace stream_bench

// ============================================================
// Statistics
// ============================================================

namespace stream_bench
{
    using clock_type = std::chrono::steady_clock;

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

    bench_stats compute_stats(std::vector<int64_t> samples, size_t blob_size, size_t trim_count)
    {
        bench_stats s{};
        s.blob_size = blob_size;
        if (samples.size() < trim_count * 2)
            return s;

        std::sort(samples.begin(), samples.end());
        const size_t b = trim_count;
        const size_t e = samples.size() - trim_count;
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

    struct stress_stats
    {
        uint64_t ops_sent = 0;
        uint64_t bytes_sent = 0;
        uint64_t ops_recvd = 0;
        uint64_t bytes_recvd = 0;
        uint64_t recv_timeouts = 0;
        double elapsed_ms = 0.0;
        size_t blob_size = 0;
        bool valid = false;

        double send_mbps() const { return elapsed_ms > 0.0 ? bytes_sent / elapsed_ms / 1000.0 : 0.0; }
        double recv_mbps() const { return elapsed_ms > 0.0 ? bytes_recvd / elapsed_ms / 1000.0 : 0.0; }
    };

    static const std::vector<size_t> all_blob_sizes = {
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

    static std::vector<size_t> get_blob_sizes(const bench_config& cfg)
    {
        if (cfg.blob_size_override)
            return {*cfg.blob_size_override};
        return all_blob_sizes;
    }

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
    // Standard benchmark coroutines
    // ============================================================

    // Unidirectional sender: sends count blobs, records per-send timing.
    coro::task<bench_stats> run_unidirectional_sender(std::shared_ptr<streaming::stream> stm,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        std::vector<int64_t> samples;
        samples.reserve(cfg.count);

        for (size_t i = 0; i < cfg.warmup + cfg.count; ++i)
        {
            const auto t0 = clock_type::now();
            wd.heartbeat();
            auto status = co_await stm->send(rpc::byte_span(payload));
            const auto t1 = clock_type::now();
            wd.heartbeat();

            if (!status.is_ok())
                break;

            if (i >= cfg.warmup)
                samples.push_back(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }

        stop.store(true, std::memory_order_release);
        co_return compute_stats(std::move(samples), payload.size(), cfg.count / 10);
    }

    // Drains the stream until stop is set by the sender.
    coro::task<void> run_drain(std::shared_ptr<streaming::stream> stm, const std::atomic<bool>& stop, watchdog& wd)
    {
        std::vector<uint8_t> buf(1 << 20);
        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stm->receive(rpc::mutable_byte_span(buf), std::chrono::milliseconds{10});
            wd.heartbeat();
            if (status.is_closed())
                break;
        }
    }

    // Send-reply sender: one send + full receive echo per measurement.
    coro::task<bench_stats> run_send_reply(std::shared_ptr<streaming::stream> stm,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        std::vector<int64_t> samples;
        samples.reserve(cfg.count);
        std::vector<uint8_t> recv_buf(payload.size() + 256);

        for (size_t i = 0; i < cfg.warmup + cfg.count; ++i)
        {
            const auto t0 = clock_type::now();

            wd.heartbeat();
            auto send_st = co_await stm->send(rpc::byte_span(payload));
            wd.heartbeat();
            if (!send_st.is_ok())
                break;

            // Accumulate until we have all bytes echoed back.
            size_t received = 0;
            bool failed = false;
            while (received < payload.size())
            {
                wd.heartbeat();
                auto [status, span] = co_await stm->receive(
                    rpc::mutable_byte_span(recv_buf.data() + received, recv_buf.size() - received), cfg.recv_timeout);
                wd.heartbeat();
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

            if (i >= cfg.warmup)
                samples.push_back(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
        }

        stop.store(true, std::memory_order_release);
        co_return compute_stats(std::move(samples), payload.size(), cfg.count / 10);
    }

    // Echo server: receives and echoes back until stop is set.
    coro::task<void> run_echo(std::shared_ptr<streaming::stream> stm, const std::atomic<bool>& stop, watchdog& wd)
    {
        std::vector<uint8_t> buf(1 << 20);
        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stm->receive(rpc::mutable_byte_span(buf), std::chrono::milliseconds{10});
            wd.heartbeat();
            if (status.is_closed())
                break;
            if (status.is_ok() && !span.empty())
            {
                wd.heartbeat();
                co_await stm->send(rpc::byte_span(span));
                wd.heartbeat();
            }
        }
    }

    // ============================================================
    // Stress coroutines
    // ============================================================

    // Saturates a stream for cfg.stress_duration then signals stop.
    coro::task<stress_stats> run_stress_sender(std::shared_ptr<streaming::stream> stm,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        stress_stats s;
        s.blob_size = payload.size();
        const auto t_start = clock_type::now();
        const auto t_end = t_start + cfg.stress_duration;

        while (clock_type::now() < t_end && !stop.load(std::memory_order_acquire))
        {
            auto status = co_await stm->send(rpc::byte_span(payload));
            wd.heartbeat();
            if (!status.is_ok())
                break;
            ++s.ops_sent;
            s.bytes_sent += payload.size();
        }

        stop.store(true, std::memory_order_release);
        s.elapsed_ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t_start).count());
        s.valid = true;
        co_return s;
    }

    // Drains and counts bytes received during a stress run.
    coro::task<stress_stats> run_stress_drain(
        std::shared_ptr<streaming::stream> stm, const std::atomic<bool>& stop, const bench_config& cfg, watchdog& wd)
    {
        stress_stats s;
        std::vector<uint8_t> buf(1 << 20);
        const auto t_start = clock_type::now();

        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stm->receive(rpc::mutable_byte_span(buf), cfg.recv_timeout);
            wd.heartbeat();
            if (status.is_closed())
                break;
            if (!span.empty())
            {
                ++s.ops_recvd;
                s.bytes_recvd += span.size();
            }
            else
            {
                ++s.recv_timeouts;
            }
        }

        s.elapsed_ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t_start).count());
        s.valid = true;
        co_return s;
    }

    // ============================================================
    // Reporting
    // ============================================================

    void print_unidirectional_header(const bench_config& cfg)
    {
        fmt::print("\n=== Unidirectional (send throughput) — {} sends, middle 80%, warmup {}\n", cfg.count, cfg.warmup);
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

    void print_send_reply_header(const bench_config& cfg)
    {
        fmt::print(
            "\n=== Send-Reply (round-trip latency) — {} round-trips, middle 80%, warmup {}\n", cfg.count, cfg.warmup);
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

    void print_stress_header(const bench_config& cfg)
    {
        fmt::print("\n=== Stress Test — {} s per run, watchdog {}ms, blob size {}\n",
            cfg.stress_duration.count(),
            cfg.watchdog_timeout.count() > 0 ? fmt::format("{}ms", cfg.watchdog_timeout.count()) : "disabled",
            cfg.blob_size_override ? fmt::format("{} bytes", *cfg.blob_size_override) : "4096 bytes (default)");
        fmt::print("{:-<28}+{:-<12}+{:-<14}+{:-<14}+{:-<14}+{:-<14}+{:-<12}\n", "", "", "", "", "", "", "");
        fmt::print("{:<27} | {:>10} | {:>12} | {:>12} | {:>12} | {:>12} | {:>10}\n",
            "stream_type",
            "blob_bytes",
            "send MB/s",
            "recv MB/s",
            "ops_sent",
            "ops_recvd",
            "timeouts");
        fmt::print("{:-<28}+{:-<12}+{:-<14}+{:-<14}+{:-<14}+{:-<14}+{:-<12}\n", "", "", "", "", "", "", "");
    }

    void print_stress_row(const char* type, const stress_stats& send_s, const stress_stats& recv_s)
    {
        if (!send_s.valid)
        {
            fmt::print("{:<27} | {:>10} | error\n", type, send_s.blob_size);
            return;
        }
        const char* verdict = (recv_s.recv_timeouts == 0) ? "PASS" : "WARN";
        fmt::print("{:<27} | {:>10} | {:>12.2f} | {:>12.2f} | {:>12} | {:>12} | {:>10} [{}]\n",
            type,
            send_s.blob_size,
            send_s.send_mbps(),
            recv_s.recv_mbps(),
            send_s.ops_sent,
            recv_s.ops_recvd,
            recv_s.recv_timeouts,
            verdict);
    }

    // ============================================================
    // SPSC-based benchmark helpers
    // ============================================================

    void run_spsc_based_bench(const char* name,
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        bench_stats& out_unidirectional,
        bench_stats& out_send_reply)
    {
        const std::vector<uint8_t> payload(blob_size, 0xAB);

        if (cfg.run_unidirectional)
        {
            std::atomic<bool> stop{false};
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            coro::sync_wait(coro::when_all([&]() -> coro::task<void>
                { out_unidirectional = co_await run_unidirectional_sender(side_a, payload, stop, cfg, wd); }(),
                run_drain(side_b, stop, wd)));
        }

        if (cfg.run_send_reply)
        {
            std::atomic<bool> stop{false};
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            coro::sync_wait(coro::when_all([&]() -> coro::task<void>
                { out_send_reply = co_await run_send_reply(side_a, payload, stop, cfg, wd); }(),
                run_echo(side_b, stop, wd)));
        }

        (void)name;
    }

    void run_spsc_based_stress_bench(const char* name,
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        stress_stats& out_send,
        stress_stats& out_recv)
    {
        const size_t blob_size = cfg.blob_size_override.value_or(4096);
        const std::vector<uint8_t> payload(blob_size, 0xAB);
        std::atomic<bool> stop{false};

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        coro::sync_wait(coro::when_all([&]() -> coro::task<void>
            { out_send = co_await run_stress_sender(side_a, payload, stop, cfg, wd); }(),
            [&]() -> coro::task<void> { out_recv = co_await run_stress_drain(side_b, stop, cfg, wd); }()));

        (void)name;
    }

    // ============================================================
    // TCP benchmark helpers
    // ============================================================

    void run_tcp_bench(
        size_t blob_size, uint16_t port, const bench_config& cfg, watchdog& wd, bench_stats& out_uni, bench_stats& out_reply)
    {
        const std::vector<uint8_t> payload(blob_size, 0xAB);
        const coro::net::socket_address endpoint{"127.0.0.1", port};

        auto sched_server = make_scheduler();
        auto sched_client = make_scheduler();

        if (cfg.run_unidirectional)
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
                    co_await run_drain(stm, stop, wd);
                }(),
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    co_await server_ready.wait();
                    coro::net::tcp::client client(sched_client, endpoint);
                    if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                        co_return;
                    auto stm = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                    out_uni = co_await run_unidirectional_sender(stm, payload, stop, cfg, wd);
                }()));
        }

        if (cfg.run_send_reply)
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
                    co_await run_echo(stm, stop, wd);
                }(),
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&]() -> coro::task<void>
                {
                    co_await server_ready.wait();
                    coro::net::tcp::client client(sched_client, endpoint);
                    if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                        co_return;
                    auto stm = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                    out_reply = co_await run_send_reply(stm, payload, stop, cfg, wd);
                }()));
        }

        sched_server->shutdown();
        sched_client->shutdown();
    }

    void run_tcp_stress_bench(
        uint16_t port, const bench_config& cfg, watchdog& wd, stress_stats& out_send, stress_stats& out_recv)
    {
        const size_t blob_size = cfg.blob_size_override.value_or(4096);
        const std::vector<uint8_t> payload(blob_size, 0xAB);
        const coro::net::socket_address endpoint{"127.0.0.1", port};

        auto sched_server = make_scheduler();
        auto sched_client = make_scheduler();
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
                out_recv = co_await run_stress_drain(stm, stop, cfg, wd);
            }(),
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            [&]() -> coro::task<void>
            {
                co_await server_ready.wait();
                coro::net::tcp::client client(sched_client, endpoint);
                if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                    co_return;
                auto stm = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                out_send = co_await run_stress_sender(stm, payload, stop, cfg, wd);
            }()));

        sched_server->shutdown();
        sched_client->shutdown();
    }

#ifdef __linux__
    // ============================================================
    // io_uring benchmark helpers
    // ============================================================

    void run_io_uring_bench(
        size_t blob_size, uint16_t port, const bench_config& cfg, watchdog& wd, bench_stats& out_uni, bench_stats& out_reply)
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

        if (cfg.run_unidirectional)
        {
            std::atomic<bool> stop{false};
            rpc::event ready;
            run_bench(
                stop,
                ready,
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void> { co_await run_drain(stm, stop, wd); },
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void>
                { out_uni = co_await run_unidirectional_sender(stm, payload, stop, cfg, wd); });
        }

        if (cfg.run_send_reply)
        {
            std::atomic<bool> stop{false};
            rpc::event ready;
            run_bench(
                stop,
                ready,
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void> { co_await run_echo(stm, stop, wd); },
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](std::shared_ptr<streaming::stream> stm) -> coro::task<void>
                { out_reply = co_await run_send_reply(stm, payload, stop, cfg, wd); });
        }

        sched_server->shutdown();
        sched_client->shutdown();
    }

    void run_io_uring_stress_bench(
        uint16_t port, const bench_config& cfg, watchdog& wd, stress_stats& out_send, stress_stats& out_recv)
    {
        const size_t blob_size = cfg.blob_size_override.value_or(4096);
        const std::vector<uint8_t> payload(blob_size, 0xAB);
        canopy::network_config::ip_address addr{};
        addr[0] = 127;
        addr[1] = 0;
        addr[2] = 0;
        addr[3] = 1;

        auto sched_server = make_scheduler();
        auto sched_client = make_scheduler();
        std::atomic<bool> stop{false};
        rpc::event server_ready;

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
                out_recv = co_await run_stress_drain(*maybe, stop, cfg, wd);
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
                out_send = co_await run_stress_sender(stm, payload, stop, cfg, wd);
            }()));

        sched_server->shutdown();
        sched_client->shutdown();
    }
#endif

} // namespace stream_bench

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
    using namespace stream_bench;
    using namespace bench_helpers;

    const bench_config cfg = parse_args(argc, argv);

    std::cout << "RPC++ Streaming Benchmark\n";
    std::cout << "=========================\n\n";

    // Print effective configuration
    {
        std::string stream_list = cfg.streams.empty() ? "all" : "";
        if (!cfg.streams.empty())
            for (const auto& s : cfg.streams)
                stream_list += (stream_list.empty() ? "" : ", ") + s;

        fmt::print("Configuration:\n");
        fmt::print("  streams:    {}\n", stream_list);
        fmt::print("  scenarios:  {}{}{}\n",
            cfg.run_unidirectional ? "unidirectional " : "",
            cfg.run_send_reply ? "send_reply " : "",
            cfg.run_stress ? "stress" : "");
        fmt::print("  count:      {}\n", cfg.count);
        fmt::print("  warmup:     {}\n", cfg.warmup);
        if (cfg.blob_size_override)
            fmt::print("  blob-size:  {} bytes\n", *cfg.blob_size_override);
        else
            fmt::print("  blob-size:  sweep ({} sizes)\n", all_blob_sizes.size());
        fmt::print("  timeout:    {}ms\n", cfg.recv_timeout.count());
        if (cfg.run_stress)
            fmt::print("  duration:   {}s\n", cfg.stress_duration.count());
        fmt::print("  watchdog:   {}\n",
            cfg.watchdog_timeout.count() > 0 ? fmt::format("{}ms", cfg.watchdog_timeout.count()) : "disabled");
        fmt::print("\n");
    }

    // Warn if the per-receive timeout exceeds the watchdog timeout: a legitimate slow receive
    // can fire the watchdog before it times out.  Either increase --watchdog-ms or reduce
    // --timeout-ms so that watchdog-ms > timeout-ms.
    if (cfg.watchdog_timeout.count() > 0 && cfg.recv_timeout > cfg.watchdog_timeout)
    {
        fmt::print(stderr,
            "WARNING: --timeout-ms ({}) > --watchdog-ms ({}).\n"
            "         A slow but legitimate receive can trigger the watchdog before it times out.\n"
            "         Consider: --watchdog-ms {}\n\n",
            cfg.recv_timeout.count(),
            cfg.watchdog_timeout.count(),
            cfg.recv_timeout.count() + 5000);
    }

    // Generate TLS cert once for all TLS benchmarks.
    temp_cert_pair tls_cert;
    if (!tls_cert.valid)
        std::cerr << "WARNING: TLS cert generation failed — TLS benchmarks will be skipped\n";

    // Start watchdog
    watchdog wd{cfg.watchdog_timeout};
    wd.heartbeat();

    const auto should_run = [&](const char* name) -> bool { return cfg.streams.empty() || cfg.streams.count(name) > 0; };

    const std::vector<size_t> blob_sizes = get_blob_sizes(cfg);

    // ============================================================
    // Standard benchmarks (unidirectional + send_reply)
    // ============================================================

    std::vector<std::pair<std::string, std::pair<std::vector<bench_stats>, std::vector<bench_stats>>>> all_results;

    if (cfg.run_unidirectional || cfg.run_send_reply)
    {
        auto collect = [&](const char* name, auto bench_fn)
        {
            if (!should_run(name))
                return;
            RPC_INFO("Benchmarking: {}", name);
            std::vector<bench_stats> uni;
            std::vector<bench_stats> reply;
            for (size_t blob_size : blob_sizes)
            {
                wd.set_context(fmt::format("{} blob={}B", name, blob_size));
                wd.heartbeat();
                bench_stats u{};
                bench_stats r{};
                bench_fn(blob_size, u, r);
                uni.push_back(u);
                reply.push_back(r);
            }
            all_results.push_back({name, {std::move(uni), std::move(reply)}});
        };

        // --- spsc ---
        collect("spsc",
            [&](size_t blob_size, bench_stats& uni, bench_stats& reply)
            {
                auto sched_a = make_scheduler();
                auto sched_b = make_scheduler();
                auto pipe = std::make_unique<spsc_pipe>();
                auto side_a = pipe->side_a(sched_a);
                auto side_b = pipe->side_b(sched_b);
                run_spsc_based_bench("spsc", side_a, side_b, cfg, wd, blob_size, uni, reply);
                sched_a->shutdown();
                sched_b->shutdown();
            });

        // --- tcp ---
        {
            uint16_t port = 19200;
            collect("tcp",
                [&](size_t blob_size, bench_stats& uni, bench_stats& reply)
                {
                    run_tcp_bench(blob_size, port, cfg, wd, uni, reply);
                    port += 2;
                });
        }

#ifdef __linux__
        // --- io_uring ---
        {
            uint16_t port = 19400;
            collect("io_uring",
                [&](size_t blob_size, bench_stats& uni, bench_stats& reply)
                {
                    run_io_uring_bench(blob_size, port, cfg, wd, uni, reply);
                    port += 2;
                });
        }
#endif

        // --- tls+spsc ---
        if (tls_cert.valid)
        {
            collect("tls+spsc",
                [&](size_t blob_size, bench_stats& uni, bench_stats& reply)
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

                    run_spsc_based_bench("tls+spsc", tls_a, tls_b, cfg, wd, blob_size, uni, reply);
                    sched_a->shutdown();
                    sched_b->shutdown();
                });
        }

        // --- ws+spsc ---
        collect("ws+spsc",
            [&](size_t blob_size, bench_stats& uni, bench_stats& reply)
            {
                auto sched_a = make_scheduler();
                auto sched_b = make_scheduler();
                auto pipe = std::make_unique<spsc_pipe>();
                // side_a is the WS server, side_b is the WS client
                auto ws_server = std::make_shared<streaming::websocket::stream>(pipe->side_a(sched_a));
                auto ws_client = std::make_shared<ws_client_stream>(pipe->side_b(sched_b));
                run_spsc_based_bench("ws+spsc", ws_server, ws_client, cfg, wd, blob_size, uni, reply);
                sched_a->shutdown();
                sched_b->shutdown();
            });

        // --- tls+ws+spsc ---
        if (tls_cert.valid)
        {
            collect("tls+ws+spsc",
                [&](size_t blob_size, bench_stats& uni, bench_stats& reply)
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
                    run_spsc_based_bench("tls+ws+spsc", ws_server, ws_client, cfg, wd, blob_size, uni, reply);
                    sched_a->shutdown();
                    sched_b->shutdown();
                });
        }

        // ---- Print standard results ----

        if (cfg.run_unidirectional)
        {
            print_unidirectional_header(cfg);
            for (const auto& [name, results] : all_results)
                for (const auto& s : results.first)
                    print_unidirectional_row(name.c_str(), s);
        }

        if (cfg.run_send_reply)
        {
            print_send_reply_header(cfg);
            for (const auto& [name, results] : all_results)
                for (const auto& s : results.second)
                    print_send_reply_row(name.c_str(), s);
        }
    }

    // ============================================================
    // Stress benchmarks
    // ============================================================

    if (cfg.run_stress)
    {
        std::vector<std::pair<std::string, std::pair<stress_stats, stress_stats>>> stress_results;

        auto collect_stress = [&](const char* name, auto bench_fn)
        {
            if (!should_run(name))
                return;
            RPC_INFO("Stress testing: {}", name);
            wd.set_context(fmt::format("{} stress", name));
            wd.heartbeat();
            stress_stats send_s{};
            stress_stats recv_s{};
            bench_fn(send_s, recv_s);
            stress_results.push_back({name, {std::move(send_s), std::move(recv_s)}});
        };

        // --- spsc stress ---
        collect_stress("spsc",
            [&](stress_stats& send_s, stress_stats& recv_s)
            {
                auto sched_a = make_scheduler();
                auto sched_b = make_scheduler();
                auto pipe = std::make_unique<spsc_pipe>();
                auto side_a = pipe->side_a(sched_a);
                auto side_b = pipe->side_b(sched_b);
                run_spsc_based_stress_bench("spsc", side_a, side_b, cfg, wd, send_s, recv_s);
                sched_a->shutdown();
                sched_b->shutdown();
            });

        // --- tcp stress ---
        {
            uint16_t port = 19600;
            collect_stress("tcp",
                [&](stress_stats& send_s, stress_stats& recv_s) { run_tcp_stress_bench(port, cfg, wd, send_s, recv_s); });
        }

#ifdef __linux__
        // --- io_uring stress ---
        {
            uint16_t port = 19700;
            collect_stress("io_uring",
                [&](stress_stats& send_s, stress_stats& recv_s)
                { run_io_uring_stress_bench(port, cfg, wd, send_s, recv_s); });
        }
#endif

        // --- tls+spsc stress ---
        if (tls_cert.valid)
        {
            collect_stress("tls+spsc",
                [&](stress_stats& send_s, stress_stats& recv_s)
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

                    run_spsc_based_stress_bench("tls+spsc", tls_a, tls_b, cfg, wd, send_s, recv_s);
                    sched_a->shutdown();
                    sched_b->shutdown();
                });
        }

        // --- ws+spsc stress ---
        collect_stress("ws+spsc",
            [&](stress_stats& send_s, stress_stats& recv_s)
            {
                auto sched_a = make_scheduler();
                auto sched_b = make_scheduler();
                auto pipe = std::make_unique<spsc_pipe>();
                auto ws_server = std::make_shared<streaming::websocket::stream>(pipe->side_a(sched_a));
                auto ws_client = std::make_shared<ws_client_stream>(pipe->side_b(sched_b));
                run_spsc_based_stress_bench("ws+spsc", ws_server, ws_client, cfg, wd, send_s, recv_s);
                sched_a->shutdown();
                sched_b->shutdown();
            });

        // --- tls+ws+spsc stress ---
        if (tls_cert.valid)
        {
            collect_stress("tls+ws+spsc",
                [&](stress_stats& send_s, stress_stats& recv_s)
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
                    run_spsc_based_stress_bench("tls+ws+spsc", ws_server, ws_client, cfg, wd, send_s, recv_s);
                    sched_a->shutdown();
                    sched_b->shutdown();
                });
        }

        // ---- Print stress results ----

        print_stress_header(cfg);
        for (const auto& [name, results] : stress_results)
            print_stress_row(name.c_str(), results.first, results.second);
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
