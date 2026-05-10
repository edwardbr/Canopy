/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <system_error>

#include <fmt/format.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace stream_bench
{
    namespace
    {
        const std::vector<size_t> all_blob_sizes = {
            64,
            256,
            1024,
            4096,
            16384,
            65536,
            131072,
            262144,
            524288,
            1048576,
        };

        std::filesystem::path cxx_source_directory()
        {
            auto directory = std::filesystem::path(__FILE__).parent_path();
            directory = directory.parent_path();
            directory = directory.parent_path();
            return directory;
        }

        bool starts_with(
            std::string_view value,
            std::string_view prefix)
        {
            return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
        }

        std::string_view trim(std::string_view value)
        {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            return value;
        }

        template<class Callback>
        bool for_each_csv_value(
            std::string_view value,
            Callback callback)
        {
            while (true)
            {
                auto comma = value.find(',');
                auto part = trim(value.substr(0, comma));
                if (part.empty() || !callback(part))
                    return false;

                if (comma == std::string_view::npos)
                    return true;
                value.remove_prefix(comma + 1);
            }
        }

        bool parse_size(
            std::string_view value,
            size_t& size)
        {
            value = trim(value);
            if (value.empty())
                return false;

            uint64_t multiplier = 1;
            const auto suffix = value.back();
            if (suffix == 'k' || suffix == 'K')
            {
                multiplier = 1024;
                value.remove_suffix(1);
            }
            else if (suffix == 'm' || suffix == 'M')
            {
                multiplier = 1024 * 1024;
                value.remove_suffix(1);
            }

            uint64_t parsed = 0;
            const auto* begin = value.data();
            const auto* end = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end)
                return false;

            if (parsed > std::numeric_limits<size_t>::max() / multiplier)
                return false;

            size = static_cast<size_t>(parsed * multiplier);
            return true;
        }

        bool parse_nonzero_size(
            std::string_view value,
            size_t& parsed)
        {
            parsed = 0;
            return parse_size(value, parsed) && parsed != 0;
        }

        bool parse_uint32_value(
            std::string_view value,
            uint32_t& parsed)
        {
            value = trim(value);
            if (value.empty())
                return false;

            uint64_t parsed_64 = 0;
            const auto* begin = value.data();
            const auto* end = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(begin, end, parsed_64);
            if (ec != std::errc{} || ptr != end || parsed_64 > std::numeric_limits<uint32_t>::max())
                return false;

            parsed = static_cast<uint32_t>(parsed_64);
            return true;
        }

        bool read_option_value(
            int argc,
            char** argv,
            int& index,
            std::string_view option,
            std::string_view& value)
        {
            const std::string_view arg = argv[index];
            const std::string option_with_equals = std::string(option) + "=";
            if (starts_with(arg, option_with_equals))
            {
                value = arg.substr(option_with_equals.size());
                return true;
            }

            if (arg == option && index + 1 < argc)
            {
                value = argv[++index];
                return true;
            }

            return false;
        }

        bool read_optional_option_value(
            int argc,
            char** argv,
            int& index,
            std::string_view option,
            std::string_view& value,
            bool& has_value)
        {
            has_value = false;
            const std::string_view arg = argv[index];
            const std::string option_with_equals = std::string(option) + "=";
            if (starts_with(arg, option_with_equals))
            {
                value = arg.substr(option_with_equals.size());
                has_value = !value.empty();
                return true;
            }

            if (arg == option)
            {
                if (index + 1 < argc && !starts_with(argv[index + 1], "--"))
                {
                    value = argv[++index];
                    has_value = true;
                }
                return true;
            }

            return false;
        }

        bool append_stream_filter(
            std::set<std::string>& values,
            std::string_view value)
        {
            return for_each_csv_value(
                value,
                [&values](std::string_view part)
                {
                    values.emplace(part);
                    return true;
                });
        }

        bool append_size_filter(
            std::vector<size_t>& values,
            std::string_view value)
        {
            return for_each_csv_value(
                value,
                [&values](std::string_view part)
                {
                    size_t size = 0;
                    if (!parse_size(part, size))
                        return false;
                    values.push_back(size);
                    return true;
                });
        }

        void select_scenario(
            bench_config& cfg,
            std::string_view scenario,
            bool& scenario_set)
        {
            if (!scenario_set)
            {
                cfg.run_unidirectional = false;
                cfg.run_send_reply = false;
                cfg.run_stress = false;
                scenario_set = true;
            }

            if (scenario == "unidirectional")
                cfg.run_unidirectional = true;
            else if (scenario == "send_reply")
                cfg.run_send_reply = true;
            else if (scenario == "stress")
                cfg.run_stress = true;
            else if (scenario == "all")
            {
                cfg.run_unidirectional = true;
                cfg.run_send_reply = true;
                cfg.run_stress = true;
            }
            else
                throw std::invalid_argument("unknown streaming benchmark scenario");
        }

        double mean_value(const std::vector<double>& values)
        {
            if (values.empty())
                return 0.0;

            return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
        }

        double standard_deviation(const std::vector<double>& values)
        {
            if (values.size() < 2)
                return 0.0;

            const double mean = mean_value(values);
            double sum_squares = 0.0;
            for (const auto value : values)
            {
                const double delta = value - mean;
                sum_squares += delta * delta;
            }

            return std::sqrt(sum_squares / static_cast<double>(values.size() - 1));
        }

        double median_value(std::vector<double> values)
        {
            if (values.empty())
                return 0.0;

            std::sort(values.begin(), values.end());
            const auto middle = values.size() / 2;
            if ((values.size() % 2) == 1)
                return values[middle];

            return (values[middle - 1] + values[middle]) / 2.0;
        }

        uint64_t median_uint64(std::vector<uint64_t> values)
        {
            if (values.empty())
                return 0;

            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }

        std::string escape_javascript_string(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());

            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '<':
                    escaped += "\\u003c";
                    break;
                case '>':
                    escaped += "\\u003e";
                    break;
                case '&':
                    escaped += "\\u0026";
                    break;
                default:
                    if (character < 0x20)
                        fmt::format_to(std::back_inserter(escaped), "\\u{:04x}", character);
                    else
                        escaped += static_cast<char>(character);
                    break;
                }
            }

            return escaped;
        }
    } // namespace

    double stress_stats::send_mbps() const
    {
        return elapsed_ms > 0.0 ? (static_cast<double>(bytes_sent) / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0) : 0.0;
    }

    double stress_stats::recv_mbps() const
    {
        return elapsed_ms > 0.0 ? (static_cast<double>(bytes_recvd) / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0) : 0.0;
    }

    watchdog::watchdog(std::chrono::milliseconds timeout)
        : timeout_(timeout)
        , last_ns_(now_ns())
        , stop_(false)
    {
        if (timeout_.count() > 0)
            thread_ = std::thread([this] { run(); });
    }

    watchdog::~watchdog()
    {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
    }

    void watchdog::heartbeat()
    {
        last_ns_.store(now_ns(), std::memory_order_relaxed);
    }

    void watchdog::set_context(const std::string& context)
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        context_ = context;
    }

    int64_t watchdog::now_ns()
    {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    void watchdog::run()
    {
        while (!stop_.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
            if (stop_.load(std::memory_order_relaxed))
                break;

            const int64_t elapsed_ms = (now_ns() - last_ns_.load(std::memory_order_relaxed)) / 1'000'000LL;
            if (elapsed_ms <= timeout_.count())
                continue;

            std::string context;
            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                context = context_;
            }
            fmt::print(
                stderr,
                "\n[WATCHDOG] No progress for {}ms (limit {}ms){} - aborting\n",
                elapsed_ms,
                timeout_.count(),
                context.empty() ? "" : fmt::format(" during '{}'", context));
            std::fflush(stderr);
            std::abort();
        }
    }

    std::filesystem::path default_html_report_path()
    {
        return cxx_source_directory() / "telemetry" / "reports" / "streaming-benchmark.html";
    }

    void print_usage(const char* program)
    {
        fmt::print(
            "Usage: {} [--stream <name[,name...]>] [--scenario <name[,name...]>] [--size <bytes[,bytes...]>] "
            "[--passes <count>] [--shuffle] [--seed <value>] [--html-report [path]] [--no-html-report]\n\n"
            "Stream names: spsc, tcp, io_uring, sgx_io_uring, tls+spsc, ws+spsc, tls+ws+spsc\n"
            "Scenario names: unidirectional, send_reply, stress, all\n"
            "Size examples: 64, 4096, 64k, 1m\n"
            "Aliases: --streams, --transport, --transports, --blob-size, --sizes, --report\n"
            "HTML report default: {}\n",
            program,
            default_html_report_path().string());
    }

    parse_status parse_args(
        int argc,
        char** argv,
        bench_config& cfg)
    {
        cfg.html_report_path = default_html_report_path();
        bool scenario_set = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                print_usage(argv[0]);
                return parse_status::help;
            }

            std::string_view value;
            bool has_optional_value = false;
            if (arg == "--no-html-report")
            {
                cfg.write_html_report = false;
                continue;
            }
            if (arg == "--shuffle")
            {
                cfg.shuffle = true;
                continue;
            }
            if (arg == "--ordered")
            {
                cfg.shuffle = false;
                continue;
            }
            if (read_optional_option_value(argc, argv, i, "--html-report", value, has_optional_value)
                || read_optional_option_value(argc, argv, i, "--report", value, has_optional_value)
                || read_optional_option_value(argc, argv, i, "--benchmark-report", value, has_optional_value))
            {
                cfg.write_html_report = true;
                if (has_optional_value)
                    cfg.html_report_path = std::filesystem::path(value);
                continue;
            }
            if (read_option_value(argc, argv, i, "--stream", value) || read_option_value(argc, argv, i, "--streams", value)
                || read_option_value(argc, argv, i, "--transport", value)
                || read_option_value(argc, argv, i, "--transports", value))
            {
                if (!append_stream_filter(cfg.streams, value))
                {
                    fmt::print(stderr, "Invalid stream filter: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }
            if (read_option_value(argc, argv, i, "--scenario", value)
                || read_option_value(argc, argv, i, "--scenarios", value))
            {
                try
                {
                    if (!for_each_csv_value(
                            value,
                            [&cfg, &scenario_set](std::string_view scenario)
                            {
                                select_scenario(cfg, scenario, scenario_set);
                                return true;
                            }))
                    {
                        fmt::print(stderr, "Invalid scenario filter: {}\n", value);
                        return parse_status::error;
                    }
                }
                catch (const std::invalid_argument&)
                {
                    fmt::print(stderr, "Unknown scenario '{}'\n", value);
                    return parse_status::error;
                }
                continue;
            }
            if (read_option_value(argc, argv, i, "--count", value))
            {
                if (!parse_nonzero_size(value, cfg.count))
                {
                    fmt::print(stderr, "Invalid count: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }
            if (read_option_value(argc, argv, i, "--warmup", value))
            {
                if (!parse_size(value, cfg.warmup))
                {
                    fmt::print(stderr, "Invalid warmup count: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }
            if (read_option_value(argc, argv, i, "--size", value) || read_option_value(argc, argv, i, "--sizes", value)
                || read_option_value(argc, argv, i, "--blob-size", value))
            {
                if (!append_size_filter(cfg.sizes, value))
                {
                    fmt::print(stderr, "Invalid blob size filter: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }
            if (read_option_value(argc, argv, i, "--timeout-ms", value))
            {
                size_t timeout = 0;
                if (!parse_size(value, timeout))
                {
                    fmt::print(stderr, "Invalid timeout: {}\n", value);
                    return parse_status::error;
                }
                cfg.recv_timeout = std::chrono::milliseconds{timeout};
                continue;
            }
            if (read_option_value(argc, argv, i, "--duration-s", value))
            {
                size_t seconds = 0;
                if (!parse_size(value, seconds))
                {
                    fmt::print(stderr, "Invalid stress duration: {}\n", value);
                    return parse_status::error;
                }
                cfg.stress_duration = std::chrono::seconds{seconds};
                continue;
            }
            if (read_option_value(argc, argv, i, "--watchdog-ms", value))
            {
                size_t timeout = 0;
                if (!parse_size(value, timeout))
                {
                    fmt::print(stderr, "Invalid watchdog timeout: {}\n", value);
                    return parse_status::error;
                }
                cfg.watchdog_timeout = std::chrono::milliseconds{timeout};
                continue;
            }
            if (read_option_value(argc, argv, i, "--passes", value) || read_option_value(argc, argv, i, "--repeats", value))
            {
                if (!parse_nonzero_size(value, cfg.passes))
                {
                    fmt::print(stderr, "Invalid benchmark pass count: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }
            if (read_option_value(argc, argv, i, "--seed", value))
            {
                if (!parse_uint32_value(value, cfg.shuffle_seed))
                {
                    fmt::print(stderr, "Invalid shuffle seed: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }

            fmt::print(stderr, "Unknown argument: {}\n", arg);
            print_usage(argv[0]);
            return parse_status::error;
        }

        return parse_status::ok;
    }

    void print_configuration(const bench_config& cfg)
    {
        std::string stream_list = cfg.streams.empty() ? "all" : "";
        for (const auto& stream : cfg.streams)
            stream_list += (stream_list.empty() ? "" : ", ") + stream;

        fmt::print("Configuration:\n");
        fmt::print("  streams:    {}\n", stream_list);
        fmt::print(
            "  scenarios:  {}{}{}\n",
            cfg.run_unidirectional ? "unidirectional " : "",
            cfg.run_send_reply ? "send_reply " : "",
            cfg.run_stress ? "stress" : "");
        fmt::print("  count:      {}\n", cfg.count);
        fmt::print("  warmup:     {}\n", cfg.warmup);
        fmt::print("  passes:     {}\n", cfg.passes);
        fmt::print("  order:      {} seed={}\n", cfg.shuffle ? "shuffled" : "ordered", cfg.shuffle_seed);
        fmt::print(
            "  blob-size:  {}\n", cfg.sizes.empty() ? "sweep" : fmt::format("{} selected size(s)", cfg.sizes.size()));
        fmt::print("  timeout:    {}ms\n", cfg.recv_timeout.count());
        if (cfg.run_stress)
            fmt::print("  duration:   {}s\n", cfg.stress_duration.count());
        fmt::print(
            "  watchdog:   {}\n",
            cfg.watchdog_timeout.count() > 0 ? fmt::format("{}ms", cfg.watchdog_timeout.count()) : "disabled");
        fmt::print("\n");
    }

    bool should_run_stream(
        const bench_config& cfg,
        std::string_view stream)
    {
        return cfg.streams.empty() || cfg.streams.find(std::string(stream)) != cfg.streams.end();
    }

    std::vector<size_t> get_blob_sizes(const bench_config& cfg)
    {
        if (!cfg.sizes.empty())
            return cfg.sizes;
        return all_blob_sizes;
    }

    std::vector<size_t> get_stress_blob_sizes(const bench_config& cfg)
    {
        if (!cfg.sizes.empty())
            return cfg.sizes;
        return {4096};
    }

    std::vector<benchmark_execution> make_execution_order(
        size_t job_count,
        const bench_config& cfg)
    {
        std::vector<benchmark_execution> executions;
        executions.reserve(job_count * cfg.passes);

        for (size_t pass = 0; pass < cfg.passes; ++pass)
        {
            for (size_t job_index = 0; job_index < job_count; ++job_index)
                executions.push_back(benchmark_execution{job_index, pass});
        }

        if (cfg.shuffle)
        {
            std::mt19937 rng(cfg.shuffle_seed);
            std::shuffle(executions.begin(), executions.end(), rng);
        }

        return executions;
    }

    std::shared_ptr<coro::scheduler> make_scheduler()
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = 2},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }

    uint16_t allocate_loopback_port()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        RPC_ASSERT(fd >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        const int bind_result = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        RPC_ASSERT(bind_result == 0);

        sockaddr_in bound_addr{};
        socklen_t bound_addr_len = sizeof(bound_addr);
        const int getsockname_result = ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_addr_len);
        RPC_ASSERT(getsockname_result == 0);

        ::close(fd);
        return ntohs(bound_addr.sin_port);
    }

    bench_stats compute_stats(
        std::vector<int64_t> samples,
        size_t blob_size,
        size_t trim_count)
    {
        bench_stats stats{};
        stats.blob_size = blob_size;
        if (samples.size() < (trim_count * 2))
            return stats;

        std::sort(samples.begin(), samples.end());
        const size_t begin = trim_count;
        const size_t end = samples.size() - trim_count;
        const size_t count = end - begin;
        if (count == 0)
            return stats;

        const auto mid_begin = samples.begin() + static_cast<long>(begin);
        const auto mid_end = samples.begin() + static_cast<long>(end);
        const auto sum = std::accumulate(mid_begin, mid_end, int64_t{0});
        stats.avg = static_cast<double>(sum) / static_cast<double>(count);
        stats.min = static_cast<double>(*mid_begin);
        stats.max = static_cast<double>(*(mid_end - 1));
        stats.p50 = static_cast<double>(*(mid_begin + static_cast<long>((count * 50) / 100)));
        stats.p90 = static_cast<double>(*(mid_begin + static_cast<long>((count * 90) / 100)));
        stats.p95 = static_cast<double>(*(mid_begin + static_cast<long>((count * 95) / 100)));
        stats.valid = true;
        return stats;
    }

    std::vector<standard_result_row> aggregate_standard_samples(
        const std::vector<standard_result_sample>& samples,
        bool use_unidirectional)
    {
        std::vector<standard_result_row> rows;
        for (const auto& sample : samples)
        {
            const auto already_added = std::find_if(
                rows.begin(),
                rows.end(),
                [&sample](const standard_result_row& row)
                { return row.stream == sample.stream && row.blob_size == sample.blob_size; });
            if (already_added != rows.end())
                continue;

            standard_result_row row;
            row.stream = sample.stream;
            row.blob_size = sample.blob_size;
            row.stats.blob_size = sample.blob_size;

            std::vector<double> avg_values;
            std::vector<double> min_values;
            std::vector<double> max_values;
            std::vector<double> p50_values;
            std::vector<double> p90_values;
            std::vector<double> p95_values;

            for (const auto& candidate : samples)
            {
                if (candidate.stream != sample.stream || candidate.blob_size != sample.blob_size)
                    continue;

                ++row.samples;
                const auto& stats = use_unidirectional ? candidate.unidirectional : candidate.send_reply;
                if (!stats.valid)
                {
                    ++row.failures;
                    continue;
                }

                avg_values.push_back(stats.avg);
                min_values.push_back(stats.min);
                max_values.push_back(stats.max);
                p50_values.push_back(stats.p50);
                p90_values.push_back(stats.p90);
                p95_values.push_back(stats.p95);
            }

            if (!avg_values.empty())
            {
                row.stats.avg = median_value(avg_values);
                row.stats.min = *std::min_element(min_values.begin(), min_values.end());
                row.stats.max = *std::max_element(max_values.begin(), max_values.end());
                row.stats.p50 = median_value(p50_values);
                row.stats.p90 = median_value(p90_values);
                row.stats.p95 = median_value(p95_values);
                row.stats.valid = true;
                row.avg_mean = mean_value(avg_values);
                row.avg_stddev = standard_deviation(avg_values);
            }

            rows.push_back(row);
        }

        std::sort(
            rows.begin(),
            rows.end(),
            [](const standard_result_row& left, const standard_result_row& right)
            {
                if (left.stream != right.stream)
                    return left.stream < right.stream;
                return left.blob_size < right.blob_size;
            });
        return rows;
    }

    std::vector<stress_result_row> aggregate_stress_samples(const std::vector<stress_result_sample>& samples)
    {
        std::vector<stress_result_row> rows;
        for (const auto& sample : samples)
        {
            const auto already_added = std::find_if(
                rows.begin(),
                rows.end(),
                [&sample](const stress_result_row& row)
                { return row.stream == sample.stream && row.blob_size == sample.blob_size; });
            if (already_added != rows.end())
                continue;

            stress_result_row row;
            row.stream = sample.stream;
            row.blob_size = sample.blob_size;
            row.send.blob_size = sample.blob_size;
            row.recv.blob_size = sample.blob_size;

            std::vector<double> send_mbps_values;
            std::vector<double> recv_mbps_values;
            std::vector<uint64_t> send_ops_values;
            std::vector<uint64_t> recv_ops_values;
            std::vector<uint64_t> timeout_values;

            for (const auto& candidate : samples)
            {
                if (candidate.stream != sample.stream || candidate.blob_size != sample.blob_size)
                    continue;

                ++row.samples;
                if (!candidate.send.valid)
                {
                    ++row.failures;
                    continue;
                }

                send_mbps_values.push_back(candidate.send.send_mbps());
                recv_mbps_values.push_back(candidate.recv.recv_mbps());
                send_ops_values.push_back(candidate.send.ops_sent);
                recv_ops_values.push_back(candidate.recv.ops_recvd);
                timeout_values.push_back(candidate.recv.recv_timeouts);
            }

            if (!send_mbps_values.empty())
            {
                const auto send_mbps = median_value(send_mbps_values);
                const auto recv_mbps = median_value(recv_mbps_values);
                row.send.valid = true;
                row.recv.valid = true;
                row.send.elapsed_ms = 1000.0;
                row.recv.elapsed_ms = 1000.0;
                row.send.bytes_sent = static_cast<uint64_t>(send_mbps * 1024.0 * 1024.0);
                row.recv.bytes_recvd = static_cast<uint64_t>(recv_mbps * 1024.0 * 1024.0);
                row.send.ops_sent = median_uint64(send_ops_values);
                row.recv.ops_recvd = median_uint64(recv_ops_values);
                row.recv.recv_timeouts = median_uint64(timeout_values);
                row.send_mbps_mean = mean_value(send_mbps_values);
                row.send_mbps_stddev = standard_deviation(send_mbps_values);
                row.recv_mbps_mean = mean_value(recv_mbps_values);
                row.recv_mbps_stddev = standard_deviation(recv_mbps_values);
            }

            rows.push_back(row);
        }

        std::sort(
            rows.begin(),
            rows.end(),
            [](const stress_result_row& left, const stress_result_row& right)
            {
                if (left.stream != right.stream)
                    return left.stream < right.stream;
                return left.blob_size < right.blob_size;
            });
        return rows;
    }

    void print_unidirectional_header(const bench_config& cfg)
    {
        fmt::print(
            "\n=== Unidirectional (send throughput) - {} sends, sorted middle {} samples, warmup {}\n",
            cfg.count,
            cfg.count - ((cfg.count / 10) * 2),
            cfg.warmup);
        fmt::print("Units: send time in ns, MB/s = 1024*1024 bytes per second\n");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
        fmt::print(
            "{:<27} | {:>10} | {:>13} | {:>10} | {:>10} | {:>10} | {:>10} | {:>11}\n",
            "stream_type",
            "blob_bytes",
            "payload MB/s",
            "avg_ns",
            "p50_ns",
            "p90_ns",
            "p95_ns",
            "max_ns");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
    }

    void print_unidirectional_row(const standard_result_row& row)
    {
        if (!row.stats.valid)
        {
            fmt::print("{:<27} | {:>10} | error\n", row.stream, row.blob_size);
            return;
        }

        const double throughput = row.stats.avg > 0.0
                                      ? (static_cast<double>(row.blob_size) / (1024.0 * 1024.0)) / (row.stats.avg / 1e9)
                                      : 0.0;
        fmt::print(
            "{:<27} | {:>10} | {:>13.2f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:>11.1f}\n",
            row.stream,
            row.blob_size,
            throughput,
            row.stats.avg,
            row.stats.p50,
            row.stats.p90,
            row.stats.p95,
            row.stats.max);
    }

    void print_send_reply_header(const bench_config& cfg)
    {
        fmt::print(
            "\n=== Send-Reply (round-trip latency) - {} round-trips, sorted middle {} samples, warmup {}\n",
            cfg.count,
            cfg.count - ((cfg.count / 10) * 2),
            cfg.warmup);
        fmt::print("Units: latency in us, MB/s = 1024*1024 bytes per second\n");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
        fmt::print(
            "{:<27} | {:>10} | {:>13} | {:>10} | {:>10} | {:>10} | {:>10} | {:>11}\n",
            "stream_type",
            "blob_bytes",
            "payload MB/s",
            "avg_us",
            "p50_us",
            "p90_us",
            "p95_us",
            "max_us");
        fmt::print("{:-<28}+{:-<12}+{:-<15}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "");
    }

    void print_send_reply_row(const standard_result_row& row)
    {
        if (!row.stats.valid)
        {
            fmt::print("{:<27} | {:>10} | error\n", row.stream, row.blob_size);
            return;
        }

        const double throughput = row.stats.avg > 0.0
                                      ? (static_cast<double>(row.blob_size) / (1024.0 * 1024.0)) / (row.stats.avg / 1e6)
                                      : 0.0;
        fmt::print(
            "{:<27} | {:>10} | {:>13.2f} | {:>10.2f} | {:>10.2f} | {:>10.2f} | {:>10.2f} | {:>11.2f}\n",
            row.stream,
            row.blob_size,
            throughput,
            row.stats.avg,
            row.stats.p50,
            row.stats.p90,
            row.stats.p95,
            row.stats.max);
    }

    void print_stress_header(const bench_config& cfg)
    {
        fmt::print(
            "\n=== Stress Test - {} s per run, watchdog {}\n",
            cfg.stress_duration.count(),
            cfg.watchdog_timeout.count() > 0 ? fmt::format("{}ms", cfg.watchdog_timeout.count()) : "disabled");
        fmt::print("{:-<28}+{:-<12}+{:-<14}+{:-<14}+{:-<14}+{:-<14}+{:-<12}\n", "", "", "", "", "", "", "");
        fmt::print(
            "{:<27} | {:>10} | {:>12} | {:>12} | {:>12} | {:>12} | {:>10}\n",
            "stream_type",
            "blob_bytes",
            "send MB/s",
            "recv MB/s",
            "ops_sent",
            "ops_recvd",
            "timeouts");
        fmt::print("{:-<28}+{:-<12}+{:-<14}+{:-<14}+{:-<14}+{:-<14}+{:-<12}\n", "", "", "", "", "", "", "");
    }

    void print_stress_row(const stress_result_row& row)
    {
        if (!row.send.valid)
        {
            fmt::print("{:<27} | {:>10} | error\n", row.stream, row.blob_size);
            return;
        }

        fmt::print(
            "{:<27} | {:>10} | {:>12.2f} | {:>12.2f} | {:>12} | {:>12} | {:>10}\n",
            row.stream,
            row.blob_size,
            row.send.send_mbps(),
            row.recv.recv_mbps(),
            row.send.ops_sent,
            row.recv.ops_recvd,
            row.recv.recv_timeouts);
    }

    void print_standard_quality_row(
        const char* scenario,
        const standard_result_row& row)
    {
        fmt::print(
            "{:<14} | {:<27} | {:>10} | samples {:>3} | failures {:>3} | avg mean {:>10.2f} | avg sd {:>10.2f}\n",
            scenario,
            row.stream,
            row.blob_size,
            row.samples,
            row.failures,
            row.avg_mean,
            row.avg_stddev);
    }

    void print_stress_quality_row(const stress_result_row& row)
    {
        fmt::print(
            "{:<14} | {:<27} | {:>10} | samples {:>3} | failures {:>3} | send sd {:>10.2f} MB/s | recv sd "
            "{:>10.2f} MB/s\n",
            "stress",
            row.stream,
            row.blob_size,
            row.samples,
            row.failures,
            row.send_mbps_stddev,
            row.recv_mbps_stddev);
    }

    bool write_html_report(
        const std::filesystem::path& report_path,
        const std::vector<standard_result_row>& unidirectional_rows,
        const std::vector<standard_result_row>& send_reply_rows,
        const std::vector<stress_result_row>& stress_rows)
    {
        const auto report_directory = report_path.parent_path();
        if (!report_directory.empty())
        {
            std::error_code directory_error;
            std::filesystem::create_directories(report_directory, directory_error);
            if (directory_error)
            {
                fmt::print(
                    stderr,
                    "Failed to create report directory {}: {}\n",
                    report_directory.string(),
                    directory_error.message());
                return false;
            }
        }

        std::ofstream output(report_path, std::ios::binary);
        if (!output)
        {
            fmt::print(stderr, "Failed to open report for writing: {}\n", report_path.string());
            return false;
        }

        output << R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Canopy Streaming Benchmark</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;background:#f4f7f9;margin:0;padding:20px;color:#333}
.container{max-width:1300px;margin:0 auto;background:#fff;padding:25px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,.1)}
h1{margin-top:0;color:#2c3e50;border-bottom:2px solid #eee;padding-bottom:10px;font-size:1.5rem}
.controls{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:15px;margin-bottom:25px;background:#f8f9fa;padding:15px;border-radius:6px;border:1px solid #eef0f2}
label{display:block;font-weight:700;margin-bottom:5px;font-size:.85em;color:#555}
select{width:100%;padding:8px;border:1px solid #ccd1d9;border-radius:4px;background:#fff}
.chart-container{position:relative;height:560px;width:100%}
#chart{width:100%;height:100%;display:block}
.legend{display:flex;flex-wrap:wrap;justify-content:center;gap:10px 20px;margin-top:16px;font-size:.78em;color:#34495e}
.legend-item{display:inline-flex;align-items:center;gap:7px;border:0;background:transparent;color:inherit;cursor:pointer;font:inherit;padding:2px 0}
.legend-item.hidden{opacity:.36;text-decoration:line-through}
.legend-swatch{width:12px;height:12px;border-radius:2px}
.tooltip{position:absolute;display:none;pointer-events:none;max-width:320px;padding:9px 11px;border-radius:4px;background:rgba(44,62,80,.94);color:#fff;font-size:.8em;line-height:1.4;z-index:2}
table{width:100%;min-width:900px;border-collapse:collapse;margin-top:12px}
.table-wrap{overflow-x:auto}
th,td{padding:8px 9px;border-bottom:1px solid #eef0f2;text-align:right;font-size:.82em}
th{color:#555;background:#f8f9fa}
td:first-child,td:nth-child(2),th:first-child,th:nth-child(2){text-align:left}
</style>
</head>
<body>
<div class="container">
<h1>Canopy Streaming Benchmark</h1>
<div class="controls">
<div><label for="scenario">Scenario</label><select id="scenario"><option value="unidirectional">Unidirectional</option><option value="send_reply">Send Reply</option><option value="stress">Stress</option></select></div>
<div><label for="stream">Stream</label><select id="stream"><option value="all">All Streams</option></select></div>
<div><label for="size">Blob Size</label><select id="size"><option value="all">All Sizes</option></select></div>
<div><label for="metric">Metric</label><select id="metric"><option value="throughput">Throughput MB/s</option><option value="latency">Latency (ms)</option><option value="p95">p95 Latency (ms)</option></select></div>
</div>
<div class="chart-container"><canvas id="chart"></canvas><div id="tooltip" class="tooltip"></div></div>
<div id="legend" class="legend"></div>
<details open><summary>Rows</summary><div class="table-wrap"><table><thead><tr><th>Scenario</th><th>Stream</th><th>Blob bytes</th><th>Throughput MB/s</th><th>Avg ms</th><th>P95 ms</th><th>Samples</th><th>Failures</th><th>Avg SD ms</th></tr></thead><tbody id="rows"></tbody></table></div></details>
</div>
<script>
const rows = [
)HTML";

        bool first = true;
        const auto emit_common = [&](const char* scenario, const standard_result_row& row, const char* unit)
        {
            if (!first)
                output << ",\n";
            first = false;
            const double throughput = row.stats.avg > 0.0 ? (static_cast<double>(row.blob_size) / (1024.0 * 1024.0))
                                                                / (row.stats.avg / (unit[0] == 'n' ? 1e9 : 1e6))
                                                          : 0.0;
            output << "  {"
                   << "\"scenario\":\"" << scenario << "\","
                   << "\"stream\":\"" << escape_javascript_string(row.stream) << "\","
                   << "\"blobSize\":" << row.blob_size << ","
                   << "\"throughput\":" << fmt::format("{:.6f}", throughput) << ","
                   << "\"avg\":" << fmt::format("{:.6f}", row.stats.avg) << ","
                   << "\"p95\":" << fmt::format("{:.6f}", row.stats.p95) << ","
                   << "\"samples\":" << row.samples << ","
                   << "\"failures\":" << row.failures << ","
                   << "\"avgStddev\":" << fmt::format("{:.6f}", row.avg_stddev) << ","
                   << "\"unit\":\"" << unit << "\"}";
        };

        for (const auto& row : unidirectional_rows)
            emit_common("unidirectional", row, "ns");
        for (const auto& row : send_reply_rows)
            emit_common("send_reply", row, "us");
        for (const auto& row : stress_rows)
        {
            if (!first)
                output << ",\n";
            first = false;
            output << "  {"
                   << "\"scenario\":\"stress\","
                   << "\"stream\":\"" << escape_javascript_string(row.stream) << "\","
                   << "\"blobSize\":" << row.blob_size << ","
                   << "\"throughput\":" << fmt::format("{:.6f}", row.send.send_mbps()) << ","
                   << "\"avg\":0,"
                   << "\"p95\":0,"
                   << "\"samples\":" << row.samples << ","
                   << "\"failures\":" << row.failures << ","
                   << "\"avgStddev\":" << fmt::format("{:.6f}", row.send_mbps_stddev) << ","
                   << "\"unit\":\"MB/s\"}";
        }

        output << R"HTML(
];
const colors=["#3498db","#e74c3c","#2ecc71","#f1c40f","#9b59b6","#34495e","#16a085","#d35400","#2980b9","#8e44ad"];
const hidden=new Set();
let points=[];
function uniq(v){return Array.from(new Set(v)).sort((a,b)=>String(a).localeCompare(String(b)));}
function uniqSize(v){return Array.from(new Set(v)).sort((a,b)=>a-b);}
function fmtSize(b){if(b>=1048576&&b%1048576===0)return `${b/1048576} MiB`;if(b>=1024&&b%1024===0)return `${b/1024} KiB`;return `${b} B`;}
function fmt(v){if(!Number.isFinite(v))return "N/A";if(Math.abs(v)<1)return v.toFixed(3);if(v>=1000)return v.toFixed(0);if(v>=100)return v.toFixed(1);return v.toFixed(2);}
function esc(v){return String(v).replaceAll("&","&amp;").replaceAll("<","&lt;").replaceAll(">","&gt;").replaceAll('"',"&quot;");}
function addOptions(id,values,label=(v)=>v){const s=document.getElementById(id);values.forEach(v=>s.add(new Option(label(v),v)));}
function initFilters(){addOptions("stream",uniq(rows.map(r=>r.stream)));addOptions("size",uniqSize(rows.map(r=>r.blobSize)),fmtSize);}
function filters(){return{scenario:document.getElementById("scenario").value,stream:document.getElementById("stream").value,size:document.getElementById("size").value,metric:document.getElementById("metric").value};}
function filtered(f){return rows.filter(r=>r.scenario===f.scenario&&(f.stream==="all"||r.stream===f.stream)&&(f.size==="all"||r.blobSize===Number(f.size)));}
function isLatencyRow(r){return r.unit==="ns"||r.unit==="us";}
function latencyToMilliseconds(r,value){if(r.unit==="ns")return value/1000000;if(r.unit==="us")return value/1000;return value;}
function metricUnit(f){return f.metric==="throughput"?"MB/s":"ms";}
function metricAxis(f){return f.metric==="throughput"?"Throughput (MB/s)":"Latency (ms)";}
function metricValue(r,f){if(f.metric==="throughput")return r.throughput;if(f.metric==="latency")return latencyToMilliseconds(r,r.avg);if(f.metric==="p95")return latencyToMilliseconds(r,r.p95);return 0;}
function niceMax(v){if(!Number.isFinite(v)||v<=0)return 1;const e=Math.floor(Math.log10(v));const m=Math.pow(10,e);const n=v/m;if(n<=2)return 2*m;if(n<=5)return 5*m;return 10*m;}
function canvas(){const c=document.getElementById("chart");const rect=c.getBoundingClientRect();const ratio=window.devicePixelRatio||1;c.width=Math.max(1,Math.floor(rect.width*ratio));c.height=Math.max(1,Math.floor(rect.height*ratio));const ctx=c.getContext("2d");ctx.setTransform(ratio,0,0,ratio,0,0);return{c,ctx,w:rect.width,h:rect.height};}
function draw(){const f=filters();const data=filtered(f);const sizes=f.size==="all"?uniqSize(data.map(r=>r.blobSize)):[Number(f.size)];const streams=uniq(data.map(r=>r.stream));const sets=streams.map((s,i)=>({label:s,color:colors[i%colors.length],points:sizes.map(size=>data.find(r=>r.stream===s&&r.blobSize===size)||null)}));const {ctx,w,h}=canvas();const margin={left:78,right:28,top:28,bottom:72};const pw=Math.max(1,w-margin.left-margin.right);const ph=Math.max(1,h-margin.top-margin.bottom);const visible=sets.filter(s=>!hidden.has(s.label));const values=visible.flatMap(s=>s.points.filter(Boolean).map(r=>metricValue(r,f))).filter(v=>v>0);const ymax=niceMax(Math.max(0,...values)*1.08);points=[];ctx.clearRect(0,0,w,h);ctx.fillStyle="#fff";ctx.fillRect(0,0,w,h);ctx.font="12px -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif";if(values.length===0){ctx.fillStyle="#95a5a6";ctx.textAlign="center";ctx.fillText("No successful rows match the selected filters.",w/2,h/2);legend(sets);table(data);return;}const xFor=i=>sizes.length<=1?margin.left+pw/2:margin.left+(i/(sizes.length-1))*pw;const yFor=v=>margin.top+ph-(v/ymax)*ph;ctx.strokeStyle="#e5e9ef";ctx.fillStyle="#777";ctx.textAlign="right";ctx.textBaseline="middle";for(let i=0;i<=5;++i){const v=ymax*i/5;const y=yFor(v);ctx.beginPath();ctx.moveTo(margin.left,y);ctx.lineTo(w-margin.right,y);ctx.stroke();ctx.fillText(fmt(v),margin.left-10,y);}ctx.strokeStyle="#ccd1d9";ctx.beginPath();ctx.moveTo(margin.left,margin.top);ctx.lineTo(margin.left,margin.top+ph);ctx.lineTo(w-margin.right,margin.top+ph);ctx.stroke();ctx.fillStyle="#555";ctx.textAlign="center";ctx.textBaseline="top";sizes.forEach((size,i)=>{const x=xFor(i);ctx.strokeStyle="#eef0f2";ctx.beginPath();ctx.moveTo(x,margin.top);ctx.lineTo(x,margin.top+ph);ctx.stroke();ctx.save();ctx.translate(x,margin.top+ph+18);if(sizes.length>7){ctx.rotate(-Math.PI/6);ctx.textAlign="right";}ctx.fillText(fmtSize(size),0,0);ctx.restore();});ctx.font="bold 12px -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif";ctx.fillStyle="#555";ctx.save();ctx.translate(18,margin.top+ph/2);ctx.rotate(-Math.PI/2);ctx.textAlign="center";ctx.textBaseline="bottom";ctx.fillText(metricAxis(f),0,0);ctx.restore();visible.forEach(set=>{ctx.strokeStyle=set.color;ctx.lineWidth=2;ctx.beginPath();let open=false;set.points.forEach((r,i)=>{if(!r){open=false;return;}const x=xFor(i);const y=yFor(metricValue(r,f));if(!open){ctx.moveTo(x,y);open=true;}else ctx.lineTo(x,y);});ctx.stroke();set.points.forEach((r,i)=>{if(!r)return;const x=xFor(i);const y=yFor(metricValue(r,f));ctx.fillStyle=set.color;ctx.strokeStyle="#fff";ctx.beginPath();ctx.arc(x,y,4,0,Math.PI*2);ctx.fill();ctx.stroke();points.push({x,y,row:r,label:set.label,value:metricValue(r,f)});});});legend(sets);table(data);}
function legend(sets){const l=document.getElementById("legend");l.textContent="";sets.forEach(s=>{const b=document.createElement("button");b.type="button";b.className="legend-item";if(hidden.has(s.label))b.classList.add("hidden");b.onclick=()=>{hidden.has(s.label)?hidden.delete(s.label):hidden.add(s.label);draw();};const sw=document.createElement("span");sw.className="legend-swatch";sw.style.background=s.color;b.appendChild(sw);b.append(s.label);l.appendChild(b);});}
function table(data){const body=document.getElementById("rows");body.textContent="";data.slice().sort((a,b)=>a.scenario.localeCompare(b.scenario)||a.stream.localeCompare(b.stream)||a.blobSize-b.blobSize).forEach(r=>{const tr=document.createElement("tr");[r.scenario,r.stream,String(r.blobSize),fmt(r.throughput),isLatencyRow(r)&&r.avg?fmt(latencyToMilliseconds(r,r.avg)):"",isLatencyRow(r)&&r.p95?fmt(latencyToMilliseconds(r,r.p95)):"",String(r.samples),String(r.failures),isLatencyRow(r)?fmt(latencyToMilliseconds(r,r.avgStddev)):""].forEach(v=>{const td=document.createElement("td");td.textContent=v;tr.appendChild(td);});body.appendChild(tr);});}
document.getElementById("chart").addEventListener("mousemove",e=>{const t=document.getElementById("tooltip");const rect=e.currentTarget.getBoundingClientRect();const x=e.clientX-rect.left,y=e.clientY-rect.top;let n=null,d=Infinity;points.forEach(p=>{const dist=Math.hypot(p.x-x,p.y-y);if(dist<d){n=p;d=dist;}});if(!n||d>14){t.style.display="none";return;}const f=filters();const sd=isLatencyRow(n.row)?`<br>Latency SD: ${esc(fmt(latencyToMilliseconds(n.row,n.row.avgStddev)))} ms`:"";t.innerHTML=`<strong>${esc(n.label)}</strong><br>Blob: ${esc(fmtSize(n.row.blobSize))}<br>Value: ${esc(fmt(n.value))} ${esc(metricUnit(f))}<br>Samples: ${esc(n.row.samples)}${sd}`;t.style.left=`${Math.min(x+16,rect.width-330)}px`;t.style.top=`${Math.max(8,y-18)}px`;t.style.display="block";});
document.getElementById("chart").addEventListener("mouseleave",()=>document.getElementById("tooltip").style.display="none");
["scenario","stream","size","metric"].forEach(id=>document.getElementById(id).addEventListener("change",draw));
window.addEventListener("resize",draw);
initFilters();draw();
</script>
</body>
</html>
)HTML";

        output.flush();
        return static_cast<bool>(output);
    }

    coro::task<bench_stats> run_unidirectional_sender(
        std::shared_ptr<streaming::stream> stream,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        std::vector<int64_t> samples;
        samples.reserve(cfg.count);

        for (size_t i = 0; i < cfg.warmup + cfg.count; ++i)
        {
            const auto start = clock_type::now();
            wd.heartbeat();
            auto status = co_await stream->send(rpc::byte_span(payload));
            const auto end = clock_type::now();
            wd.heartbeat();
            if (!status.is_ok())
                break;

            if (i >= cfg.warmup)
                samples.push_back(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
        }

        stop.store(true, std::memory_order_release);
        co_return compute_stats(std::move(samples), payload.size(), cfg.count / 10);
    }

    coro::task<void> run_drain(
        std::shared_ptr<streaming::stream> stream,
        const std::atomic<bool>& stop,
        watchdog& wd)
    {
        std::vector<uint8_t> buffer(1 << 20);
        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stream->receive(rpc::mutable_byte_span(buffer), std::chrono::milliseconds{10});
            wd.heartbeat();
            if (status.is_closed())
                break;
            (void)span;
        }
    }

    coro::task<bench_stats> run_send_reply(
        std::shared_ptr<streaming::stream> stream,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        std::vector<int64_t> samples;
        samples.reserve(cfg.count);
        std::vector<uint8_t> recv_buffer(payload.size() + 256);

        for (size_t i = 0; i < cfg.warmup + cfg.count; ++i)
        {
            const auto start = clock_type::now();
            wd.heartbeat();
            auto send_status = co_await stream->send(rpc::byte_span(payload));
            wd.heartbeat();
            if (!send_status.is_ok())
                break;

            size_t received = 0;
            bool failed = false;
            while (received < payload.size())
            {
                auto [status, span] = co_await stream->receive(
                    rpc::mutable_byte_span(recv_buffer.data() + received, recv_buffer.size() - received), cfg.recv_timeout);
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

            const auto end = clock_type::now();
            if (i >= cfg.warmup)
                samples.push_back(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
        }

        stop.store(true, std::memory_order_release);
        co_return compute_stats(std::move(samples), payload.size(), cfg.count / 10);
    }

    coro::task<void> run_echo(
        std::shared_ptr<streaming::stream> stream,
        const std::atomic<bool>& stop,
        watchdog& wd)
    {
        std::vector<uint8_t> buffer(1 << 20);
        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stream->receive(rpc::mutable_byte_span(buffer), std::chrono::milliseconds{10});
            wd.heartbeat();
            if (status.is_closed())
                break;
            if (status.is_ok() && !span.empty())
            {
                co_await stream->send(rpc::byte_span(span));
                wd.heartbeat();
            }
        }
    }

    coro::task<stress_stats> run_stress_sender(
        std::shared_ptr<streaming::stream> stream,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        stress_stats stats;
        stats.blob_size = payload.size();
        const auto start = clock_type::now();
        const auto end = start + cfg.stress_duration;

        while (clock_type::now() < end && !stop.load(std::memory_order_acquire))
        {
            auto status = co_await stream->send(rpc::byte_span(payload));
            wd.heartbeat();
            if (!status.is_ok())
                break;
            ++stats.ops_sent;
            stats.bytes_sent += payload.size();
        }

        stop.store(true, std::memory_order_release);
        stats.elapsed_ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count());
        stats.valid = true;
        co_return stats;
    }

    coro::task<stress_stats> run_stress_drain(
        std::shared_ptr<streaming::stream> stream,
        const std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd)
    {
        stress_stats stats;
        std::vector<uint8_t> buffer(1 << 20);
        const auto start = clock_type::now();

        while (!stop.load(std::memory_order_acquire))
        {
            auto [status, span] = co_await stream->receive(rpc::mutable_byte_span(buffer), cfg.recv_timeout);
            wd.heartbeat();
            if (status.is_closed())
                break;
            if (!span.empty())
            {
                ++stats.ops_recvd;
                stats.bytes_recvd += span.size();
            }
            else
            {
                ++stats.recv_timeouts;
            }
        }

        stats.elapsed_ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count());
        stats.valid = true;
        co_return stats;
    }

    void run_paired_stream_bench(
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        bench_stats& out_unidirectional,
        bench_stats& out_send_reply)
    {
        const std::vector<uint8_t> payload(blob_size, 0xab);

        if (cfg.run_unidirectional)
        {
            std::atomic<bool> stop{false};
            coro::sync_wait(
                coro::when_all(
                    [&]() -> coro::task<void>
                    { out_unidirectional = co_await run_unidirectional_sender(side_a, payload, stop, cfg, wd); }(),
                    run_drain(side_b, stop, wd)));
        }

        if (cfg.run_send_reply)
        {
            std::atomic<bool> stop{false};
            coro::sync_wait(
                coro::when_all(
                    [&]() -> coro::task<void>
                    { out_send_reply = co_await run_send_reply(side_a, payload, stop, cfg, wd); }(),
                    run_echo(side_b, stop, wd)));
        }
    }

    void run_paired_stream_stress_bench(
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        stress_stats& out_send,
        stress_stats& out_recv)
    {
        const std::vector<uint8_t> payload(blob_size, 0xab);
        std::atomic<bool> stop{false};
        coro::sync_wait(
            coro::when_all(
                [&]() -> coro::task<void> { out_send = co_await run_stress_sender(side_a, payload, stop, cfg, wd); }(),
                [&]() -> coro::task<void> { out_recv = co_await run_stress_drain(side_b, stop, cfg, wd); }()));
    }
}
