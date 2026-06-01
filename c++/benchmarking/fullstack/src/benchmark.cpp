/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__GLIBC__)
#  include <malloc.h>
#endif

#include <fmt/format.h>

namespace
{
    void tune_allocator_for_large_payload_benchmarks()
    {
#if defined(__GLIBC__)
        constexpr int large_payload_allocator_threshold = 16 * 1024 * 1024;
        mallopt(M_MMAP_THRESHOLD, large_payload_allocator_threshold);
        mallopt(M_TRIM_THRESHOLD, large_payload_allocator_threshold);
#endif
    }

    std::filesystem::path cxx_source_directory()
    {
        auto directory = std::filesystem::path(__FILE__).parent_path();
        directory = directory.parent_path();
        directory = directory.parent_path();
        directory = directory.parent_path();
        return directory;
    }

    std::filesystem::path default_html_report_path()
    {
        return cxx_source_directory() / "telemetry" / "reports" / "benchmark.html";
    }

    struct benchmark_filters
    {
        std::vector<std::string> transports;
        std::vector<std::string> formats;
        std::vector<size_t> sizes;
        bool write_html_report = true;
        std::filesystem::path html_report_path = default_html_report_path();
        size_t passes = 1;
        bool shuffle = false;
        uint32_t shuffle_seed = 0xc0ffeeu;
    };

    struct benchmark_report_row
    {
        std::string transport;
        std::string format;
        size_t blob_size = 0;
        int error = rpc::error::OK();
        comprehensive::v1::benchmark_stats stats{};
        size_t samples = 1;
        size_t failures = 0;
        double avg_us_mean = 0.0;
        double avg_us_stddev = 0.0;
    };

    struct benchmark_job
    {
        std::string transport;
        std::string format;
        size_t blob_size = 0;
        std::function<comprehensive::v1::benchmark_result()> run;
    };

    struct benchmark_execution
    {
        size_t job_index = 0;
        size_t pass = 0;
    };

    struct io_uring_benchmark_variant
    {
        const char* name;
        bool use_proactor;
        uint32_t host_buffer_size;
    };

    enum class parse_status
    {
        ok,
        help,
        error
    };

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

    bool append_string_filter(
        std::vector<std::string>& values,
        std::string_view value)
    {
        return for_each_csv_value(
            value,
            [&values](std::string_view part)
            {
                values.emplace_back(part);
                return true;
            });
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

    bool parse_size_value(
        std::string_view value,
        size_t& parsed)
    {
        parsed = 0;
        if (!parse_size(value, parsed))
            return false;

        return parsed != 0;
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

    bool contains(
        const std::vector<std::string>& values,
        std::string_view value)
    {
        if (values.empty())
            return true;

        return std::find(values.begin(), values.end(), value) != values.end();
    }

    bool contains(
        const std::vector<size_t>& values,
        size_t value)
    {
        if (values.empty())
            return true;

        return std::find(values.begin(), values.end(), value) != values.end();
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

    void print_usage()
    {
        fmt::print(
            "Usage: benchmark [--transport <name[,name...]>] [--size <bytes[,bytes...]>] "
            "[--format <name[,name...]>] [--passes <count>] [--shuffle] [--seed <value>] "
            "[--html-report [path]] [--no-html-report]\n\n"
            "Transport names: local");
#ifndef CANOPY_BUILD_COROUTINE
        fmt::print(", dll");
#else
        fmt::print(
            ", unshared_scheduler_dll, shared_scheduler_dll, ipc_direct, ipc_dll, spsc, io_uring, "
            "io_uring_proactor_4k, io_uring_proactor_64k, io_uring_cooperative_4k, io_uring_cooperative_64k");
#  ifdef CANOPY_BENCHMARK_SGX_COROUTINE
        fmt::print(
            ", sgx_io_uring, sgx_io_uring_proactor_4k, sgx_io_uring_proactor_64k, "
            "sgx_io_uring_cooperative_4k, sgx_io_uring_cooperative_64k, sgx_io_uring_pair, "
            "sgx_io_uring_pair_proactor_4k, sgx_io_uring_pair_proactor_64k, "
            "sgx_io_uring_pair_cooperative_4k, sgx_io_uring_pair_cooperative_64k");
#  endif
#endif
        fmt::print(", tcp");
#ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
        fmt::print(", tls+tcp");
#endif
#ifdef CANOPY_BUILD_WEBSOCKET
        fmt::print(", ws+tcp");
#  ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
        fmt::print(", tls+ws+tcp");
#  endif
#endif
        fmt::print("\nFormat names: yas_binary, yas_compressed");
#ifdef CANOPY_BUILD_NANOPB
        fmt::print(", nanopb");
#endif
#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
        fmt::print(", protocol_buffers");
#endif
        fmt::print(
            "\nSize examples: 64, 4096, 64k, 1m\n"
            "Measurement quality: --passes repeats each selected row, --shuffle randomizes row order, "
            "--seed makes shuffled order reproducible\n"
            "HTML report default: {}\n"
            "Aliases: --transports, --blob-size, --sizes, --encoding, --serialisation, --serialization, --report\n",
            default_html_report_path().string());
    }

    parse_status parse_filters(
        int argc,
        char** argv,
        benchmark_filters& filters)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                print_usage();
                return parse_status::help;
            }

            std::string_view value;
            bool has_optional_value = false;
            if (arg == "--no-html-report")
            {
                filters.write_html_report = false;
                continue;
            }

            if (arg == "--shuffle")
            {
                filters.shuffle = true;
                continue;
            }

            if (arg == "--ordered")
            {
                filters.shuffle = false;
                continue;
            }

            if (read_optional_option_value(argc, argv, i, "--html-report", value, has_optional_value)
                || read_optional_option_value(argc, argv, i, "--report", value, has_optional_value)
                || read_optional_option_value(argc, argv, i, "--benchmark-report", value, has_optional_value))
            {
                filters.write_html_report = true;
                if (has_optional_value)
                    filters.html_report_path = std::filesystem::path(value);
                continue;
            }

            if (read_option_value(argc, argv, i, "--transport", value)
                || read_option_value(argc, argv, i, "--transports", value))
            {
                if (!append_string_filter(filters.transports, value))
                {
                    fmt::print(stderr, "Invalid transport filter: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }

            if (read_option_value(argc, argv, i, "--format", value) || read_option_value(argc, argv, i, "--encoding", value)
                || read_option_value(argc, argv, i, "--serialisation", value)
                || read_option_value(argc, argv, i, "--serialization", value))
            {
                if (!append_string_filter(filters.formats, value))
                {
                    fmt::print(stderr, "Invalid format filter: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }

            if (read_option_value(argc, argv, i, "--size", value) || read_option_value(argc, argv, i, "--sizes", value)
                || read_option_value(argc, argv, i, "--blob-size", value))
            {
                if (!append_size_filter(filters.sizes, value))
                {
                    fmt::print(stderr, "Invalid size filter: {}\n", value);
                    return parse_status::error;
                }
                continue;
            }

            if (read_option_value(argc, argv, i, "--passes", value) || read_option_value(argc, argv, i, "--repeats", value))
            {
                size_t passes = 0;
                if (!parse_size_value(value, passes))
                {
                    fmt::print(stderr, "Invalid benchmark pass count: {}\n", value);
                    return parse_status::error;
                }
                filters.passes = passes;
                continue;
            }

            if (read_option_value(argc, argv, i, "--seed", value))
            {
                uint32_t seed = 0;
                if (!parse_uint32_value(value, seed))
                {
                    fmt::print(stderr, "Invalid benchmark shuffle seed: {}\n", value);
                    return parse_status::error;
                }
                filters.shuffle_seed = seed;
                continue;
            }

            fmt::print(stderr, "Unknown benchmark option: {}\n", arg);
            print_usage();
            return parse_status::error;
        }

        return parse_status::ok;
    }

    bool should_run_transport(
        const benchmark_filters& filters,
        std::string_view transport)
    {
        return contains(filters.transports, transport);
    }

    bool should_run_format(
        const benchmark_filters& filters,
        std::string_view format)
    {
        return contains(filters.formats, format);
    }

    bool should_run_size(
        const benchmark_filters& filters,
        size_t size)
    {
        return contains(filters.sizes, size);
    }

#ifdef CANOPY_BUILD_COROUTINE
    bool should_run_tcp_coroutine_variant(
        const benchmark_filters& filters,
        std::string_view transport)
    {
        if (filters.transports.empty())
            return true;

        return std::find(filters.transports.begin(), filters.transports.end(), "tcp_coroutine") != filters.transports.end()
               || std::find(filters.transports.begin(), filters.transports.end(), transport) != filters.transports.end();
    }
#endif

#ifdef CANOPY_BENCHMARK_SGX_COROUTINE
    bool should_run_sgx_io_uring_variant(
        const benchmark_filters& filters,
        std::string_view transport)
    {
        if (filters.transports.empty())
            return true;

        return std::find(filters.transports.begin(), filters.transports.end(), "sgx_io_uring") != filters.transports.end()
               || std::find(filters.transports.begin(), filters.transports.end(), transport) != filters.transports.end();
    }

    bool should_run_sgx_io_uring_pair_variant(
        const benchmark_filters& filters,
        std::string_view transport)
    {
        if (filters.transports.empty())
            return true;

        return std::find(filters.transports.begin(), filters.transports.end(), "sgx_io_uring_pair")
                   != filters.transports.end()
               || std::find(filters.transports.begin(), filters.transports.end(), transport) != filters.transports.end();
    }
#endif

    void print_error_row(
        const char* transport,
        const char* format,
        size_t blob_size,
        int error)
    {
        fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", transport, format, blob_size, error);
    }

    double payload_bandwidth_mb_per_second(
        size_t blob_size,
        const comprehensive::v1::benchmark_stats& stats)
    {
        if (stats.avg_us <= 0.0)
            return 0.0;

        const double size_mb = static_cast<double>(blob_size) / (1024.0 * 1024.0);
        return size_mb / (stats.avg_us / 1e6);
    }

    double round_trip_bandwidth_mb_per_second(
        size_t blob_size,
        const comprehensive::v1::benchmark_stats& stats)
    {
        return payload_bandwidth_mb_per_second(blob_size, stats) * 2.0;
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

    void sort_report_rows(std::vector<benchmark_report_row>& rows)
    {
        std::sort(
            rows.begin(),
            rows.end(),
            [](const benchmark_report_row& left, const benchmark_report_row& right)
            {
                if (left.transport != right.transport)
                    return left.transport < right.transport;
                if (left.format != right.format)
                    return left.format < right.format;
                return left.blob_size < right.blob_size;
            });
    }

    bool same_report_key(
        const benchmark_report_row& left,
        const benchmark_report_row& right)
    {
        return left.transport == right.transport && left.format == right.format && left.blob_size == right.blob_size;
    }

    benchmark_report_row aggregate_report_group(const std::vector<benchmark_report_row>& rows)
    {
        RPC_ASSERT(!rows.empty());

        benchmark_report_row aggregate = rows.front();
        aggregate.samples = rows.size();
        aggregate.failures = 0;
        aggregate.error = rows.front().error;
        aggregate.stats = {};
        aggregate.avg_us_mean = 0.0;
        aggregate.avg_us_stddev = 0.0;

        std::vector<double> avg_values;
        std::vector<double> min_values;
        std::vector<double> max_values;
        std::vector<double> p50_values;
        std::vector<double> p90_values;
        std::vector<double> p95_values;

        for (const auto& row : rows)
        {
            if (row.error != rpc::error::OK())
            {
                ++aggregate.failures;
                continue;
            }

            avg_values.push_back(row.stats.avg_us);
            min_values.push_back(row.stats.min_us);
            max_values.push_back(row.stats.max_us);
            p50_values.push_back(row.stats.p50_us);
            p90_values.push_back(row.stats.p90_us);
            p95_values.push_back(row.stats.p95_us);
        }

        if (avg_values.empty())
            return aggregate;

        aggregate.error = rpc::error::OK();
        aggregate.stats.avg_us = median_value(avg_values);
        aggregate.stats.min_us = *std::min_element(min_values.begin(), min_values.end());
        aggregate.stats.max_us = *std::max_element(max_values.begin(), max_values.end());
        aggregate.stats.p50_us = median_value(p50_values);
        aggregate.stats.p90_us = median_value(p90_values);
        aggregate.stats.p95_us = median_value(p95_values);
        aggregate.avg_us_mean = mean_value(avg_values);
        aggregate.avg_us_stddev = standard_deviation(avg_values);
        return aggregate;
    }

    std::vector<benchmark_report_row> aggregate_report_rows(std::vector<benchmark_report_row> rows)
    {
        sort_report_rows(rows);

        std::vector<benchmark_report_row> aggregates;
        for (size_t i = 0; i < rows.size();)
        {
            size_t end = i + 1;
            while (end < rows.size() && same_report_key(rows[i], rows[end]))
                ++end;

            std::vector<benchmark_report_row> group(
                rows.begin() + static_cast<long>(i), rows.begin() + static_cast<long>(end));
            aggregates.push_back(aggregate_report_group(group));
            i = end;
        }

        return aggregates;
    }

    void record_benchmark_result(
        std::vector<benchmark_report_row>& report_rows,
        const char* transport,
        const char* format,
        size_t blob_size,
        const comprehensive::v1::benchmark_result& result,
        bool print_row)
    {
        benchmark_report_row row{transport, format, blob_size, result.error, result.stats};
        row.failures = result.error == rpc::error::OK() ? 0 : 1;
        row.avg_us_mean = result.stats.avg_us;
        report_rows.push_back(row);

        if (!print_row)
            return;

        if (result.error == rpc::error::OK())
            print_stats(transport, format, blob_size, result.stats);
        else
            print_error_row(transport, format, blob_size, result.error);
    }

    void print_report_row(const benchmark_report_row& row)
    {
        if (row.error == rpc::error::OK())
            print_stats(row.transport.c_str(), row.format.c_str(), row.blob_size, row.stats);
        else
            print_error_row(row.transport.c_str(), row.format.c_str(), row.blob_size, row.error);
    }

    void print_quality_row(const benchmark_report_row& row)
    {
        fmt::print(
            "{:>28} | {:>18} | {:>9} | samples {:>3} | failures {:>3} | avg mean {:>8.2f} us | avg sd {:>8.2f} "
            "us\n",
            row.transport,
            row.format,
            row.blob_size,
            row.samples,
            row.failures,
            row.avg_us_mean,
            row.avg_us_stddev);
    }

    template<class Runner>
    void add_selected_transport_jobs(
        std::vector<benchmark_job>& jobs,
        const benchmark_filters& filters,
        const std::vector<comprehensive::v1::encoding_info>& encodings,
        const std::vector<size_t>& blob_sizes,
        const char* transport,
        Runner runner)
    {
        for (const auto& enc : encodings)
        {
            if (!should_run_format(filters, enc.name))
                continue;

            for (const auto blob_size : blob_sizes)
            {
                if (!should_run_size(filters, blob_size))
                    continue;

                jobs.push_back(
                    benchmark_job{transport,
                        enc.name,
                        blob_size,
                        [runner, encoding = enc.enc, blob_size]() { return runner(encoding, blob_size); }});
            }
        }
    }

    template<class Runner>
    void add_transport_jobs(
        std::vector<benchmark_job>& jobs,
        const benchmark_filters& filters,
        const std::vector<comprehensive::v1::encoding_info>& encodings,
        const std::vector<size_t>& blob_sizes,
        const char* transport,
        Runner runner)
    {
        if (!should_run_transport(filters, transport))
            return;

        add_selected_transport_jobs(jobs, filters, encodings, blob_sizes, transport, runner);
    }

    std::vector<benchmark_execution> make_execution_order(
        size_t job_count,
        const benchmark_filters& filters)
    {
        std::vector<benchmark_execution> executions;
        executions.reserve(job_count * filters.passes);

        for (size_t pass = 0; pass < filters.passes; ++pass)
        {
            for (size_t job_index = 0; job_index < job_count; ++job_index)
                executions.push_back(benchmark_execution{job_index, pass});
        }

        if (filters.shuffle)
        {
            std::mt19937 rng(filters.shuffle_seed);
            std::shuffle(executions.begin(), executions.end(), rng);
        }

        return executions;
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
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
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

    bool write_html_report(
        const std::filesystem::path& report_path,
        const std::vector<benchmark_report_row>& rows)
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
                    "Failed to create benchmark report directory {}: {}\n",
                    report_directory.string(),
                    directory_error.message());
                return false;
            }
        }

        std::ofstream output(report_path, std::ios::binary);
        if (!output)
        {
            fmt::print(stderr, "Failed to open benchmark report for writing: {}\n", report_path.string());
            return false;
        }

        output << R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Canopy Benchmark - Trend Analysis</title>
<style>
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background-color: #f4f7f9;
    margin: 0;
    padding: 20px;
    color: #333333;
}
.container {
    max-width: 1300px;
    margin: 0 auto;
    background: #ffffff;
    padding: 25px;
    border-radius: 8px;
    box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
}
h1 {
    margin-top: 0;
    color: #2c3e50;
    border-bottom: 2px solid #eeeeee;
    padding-bottom: 10px;
    font-size: 1.5rem;
}
.controls {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 15px;
    margin-bottom: 25px;
    background: #f8f9fa;
    padding: 15px;
    border-radius: 6px;
    border: 1px solid #eef0f2;
}
.control-group {
    display: flex;
    flex-direction: column;
}
label {
    font-weight: bold;
    margin-bottom: 5px;
    font-size: 0.85em;
    color: #555555;
}
select {
    padding: 8px;
    border: 1px solid #ccd1d9;
    border-radius: 4px;
    background: #ffffff;
    cursor: pointer;
}
.chart-container {
    position: relative;
    height: 600px;
    width: 100%;
}
#benchmarkChart {
    width: 100%;
    height: 100%;
    display: block;
}
.chart-tooltip {
    position: absolute;
    display: none;
    pointer-events: none;
    max-width: 300px;
    padding: 9px 11px;
    border-radius: 4px;
    background: rgba(44, 62, 80, 0.94);
    color: #ffffff;
    font-size: 0.8em;
    line-height: 1.4;
    z-index: 2;
}
.legend {
    display: flex;
    flex-wrap: wrap;
    justify-content: center;
    gap: 10px 20px;
    margin-top: 16px;
    font-size: 0.78em;
    color: #34495e;
}
.legend-item {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    border: 0;
    background: transparent;
    color: inherit;
    cursor: pointer;
    font: inherit;
    padding: 2px 0;
    user-select: none;
}
.legend-item.hidden {
    opacity: 0.36;
    text-decoration: line-through;
}
.legend-swatch {
    width: 12px;
    height: 12px;
    border-radius: 2px;
}
.footer-note {
    margin-top: 15px;
    font-size: 0.8em;
    color: #95a5a6;
    text-align: right;
}
details {
    margin-top: 18px;
}
summary {
    cursor: pointer;
    color: #555555;
    font-size: 0.9em;
    font-weight: bold;
}
.table-wrap {
    overflow-x: auto;
}
table {
    width: 100%;
    min-width: 900px;
    border-collapse: collapse;
    margin-top: 12px;
}
th,
td {
    padding: 8px 9px;
    border-bottom: 1px solid #eef0f2;
    text-align: right;
    font-size: 0.82em;
}
th {
    color: #555555;
    background: #f8f9fa;
}
td:first-child,
td:nth-child(2),
th:first-child,
th:nth-child(2) {
    text-align: left;
}
@media (max-width: 760px) {
    body {
        padding: 12px;
    }
    .container {
        padding: 16px;
    }
    .chart-container {
        height: 440px;
    }
}
</style>
</head>
<body>
<div class="container">
<h1>Canopy Benchmark: Performance vs Blob Size</h1>
<div class="controls">
<div class="control-group">
<label for="transportFilter">Transport</label>
<select id="transportFilter">
<option value="all">All Transports</option>
</select>
</div>
<div class="control-group">
<label for="formatFilter">Serialization Format</label>
<select id="formatFilter">
<option value="all">All Formats</option>
</select>
</div>
<div class="control-group">
<label for="sizeFilter">Blob Size Filter</label>
<select id="sizeFilter">
<option value="all">All Sizes</option>
</select>
</div>
<div class="control-group">
<label for="metricFilter">Y-Axis Metric</label>
<select id="metricFilter">
<option value="payload">Payload Throughput (MB/s)</option>
<option value="roundTrip">Round-trip Throughput (MB/s)</option>
<option value="avg">Avg Latency (ms)</option>
<option value="p95">p95 Latency (ms)</option>
</select>
</div>
</div>
<div class="chart-container">
<canvas id="benchmarkChart"></canvas>
<div id="chartTooltip" class="chart-tooltip"></div>
</div>
<div id="legend" class="legend"></div>
<div class="footer-note">
X-axis represents blob size in bytes. Latency rows use sorted trimmed samples. Failed benchmark rows are omitted from the chart.
</div>
<details>
<summary>Benchmark rows</summary>
<div class="table-wrap">
<table>
<thead>
<tr>
<th>Transport</th>
<th>Format</th>
<th>Blob bytes</th>
<th>Average us</th>
<th>P50 us</th>
<th>P90 us</th>
<th>P95 us</th>
<th>Samples</th>
<th>Failures</th>
<th>Avg SD us</th>
<th>Payload MB/s</th>
<th>Round-trip MB/s</th>
<th>Status</th>
</tr>
</thead>
<tbody id="rows"></tbody>
</table>
</div>
</details>
</div>
<script>
const benchmarkRows = [
)HTML";

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            output << "  {"
                   << "\"transport\":\"" << escape_javascript_string(row.transport) << "\","
                   << "\"format\":\"" << escape_javascript_string(row.format) << "\","
                   << "\"blobSize\":" << row.blob_size << ","
                   << "\"error\":" << row.error << ","
                   << "\"avgUs\":" << fmt::format("{:.6f}", row.stats.avg_us) << ","
                   << "\"p50Us\":" << fmt::format("{:.6f}", row.stats.p50_us) << ","
                   << "\"p90Us\":" << fmt::format("{:.6f}", row.stats.p90_us) << ","
                   << "\"p95Us\":" << fmt::format("{:.6f}", row.stats.p95_us) << ","
                   << "\"samples\":" << row.samples << ","
                   << "\"failures\":" << row.failures << ","
                   << "\"avgUsMean\":" << fmt::format("{:.6f}", row.avg_us_mean) << ","
                   << "\"avgUsStddev\":" << fmt::format("{:.6f}", row.avg_us_stddev) << ","
                   << "\"payloadMBps\":"
                   << fmt::format("{:.6f}", payload_bandwidth_mb_per_second(row.blob_size, row.stats)) << ","
                   << "\"roundTripMBps\":"
                   << fmt::format("{:.6f}", round_trip_bandwidth_mb_per_second(row.blob_size, row.stats)) << "}";
            if (i + 1 < rows.size())
                output << ",";
            output << "\n";
        }

        output << R"HTML(];

const metricDefinitions = {
    payload: { field: "payloadMBps", axis: "Throughput (MB/s)", label: "Payload Throughput (MB/s)", unit: "MB/s" },
    roundTrip: { field: "roundTripMBps", axis: "Throughput (MB/s)", label: "Round-trip Throughput (MB/s)", unit: "MB/s" },
    avg: { field: "avgUs", axis: "Latency (ms)", label: "Avg Latency (ms)", unit: "ms", scale: 0.001 },
    p95: { field: "p95Us", axis: "Latency (ms)", label: "p95 Latency (ms)", unit: "ms", scale: 0.001 }
};

const colors = [
    "#3498db", "#e74c3c", "#2ecc71", "#f1c40f", "#9b59b6",
    "#34495e", "#16a085", "#d35400", "#2980b9", "#8e44ad",
    "#27ae60", "#c0392b"
];

let chartState = {
    points: [],
    metric: "payload"
};
const hiddenSeries = new Set();

function uniqueStrings(values) {
    return Array.from(new Set(values)).sort((a, b) => String(a).localeCompare(String(b)));
}

function uniqueSizes(values) {
    return Array.from(new Set(values)).sort((a, b) => a - b);
}

function formatSize(bytes) {
    if (bytes >= 1048576 && bytes % 1048576 === 0) {
        return `${bytes / 1048576} MiB`;
    }
    if (bytes >= 1024 && bytes % 1024 === 0) {
        return `${bytes / 1024} KiB`;
    }
    return `${bytes} B`;
}

function formatNumber(value) {
    if (!Number.isFinite(value)) {
        return "N/A";
    }
    if (Math.abs(value) < 1) {
        return value.toFixed(3);
    }
    if (value >= 1000) {
        return value.toFixed(0);
    }
    if (value >= 100) {
        return value.toFixed(1);
    }
    return value.toFixed(2);
}

function escapeHtml(value) {
    return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

function addSelectOptions(selectId, values, labelCallback = (value) => value) {
    const select = document.getElementById(selectId);
    values.forEach((value) => {
        select.add(new Option(labelCallback(value), value));
    });
}

function populateFilters() {
    addSelectOptions("transportFilter", uniqueStrings(benchmarkRows.map((row) => row.transport)));
    addSelectOptions("formatFilter", uniqueStrings(benchmarkRows.map((row) => row.format)));
    addSelectOptions("sizeFilter", uniqueSizes(benchmarkRows.map((row) => row.blobSize)), formatSize);
}

function currentFilters() {
    return {
        transport: document.getElementById("transportFilter").value,
        format: document.getElementById("formatFilter").value,
        size: document.getElementById("sizeFilter").value,
        metric: document.getElementById("metricFilter").value
    };
}

function filteredRows(filters) {
    return benchmarkRows.filter((row) => {
        return (filters.transport === "all" || row.transport === filters.transport)
            && (filters.format === "all" || row.format === filters.format)
            && (filters.size === "all" || row.blobSize === Number(filters.size));
    });
}

function selectedBlobSizes(filters, rows) {
    if (filters.size !== "all") {
        return [Number(filters.size)];
    }
    return uniqueSizes(rows.map((row) => row.blobSize));
}

function buildDatasets(rows, blobSizes, metric) {
    const metricInfo = metricDefinitions[metric];
    const combinations = [];
    rows.forEach((row) => {
        if (row.error !== 0 || row[metricInfo.field] <= 0) {
            return;
        }

        const key = `${row.transport} (${row.format})`;
        if (!combinations.includes(key)) {
            combinations.push(key);
        }
    });

    return combinations.map((label, index) => {
        const splitAt = label.lastIndexOf(" (");
        const transport = label.substring(0, splitAt);
        const format = label.substring(splitAt + 2, label.length - 1);
        const points = blobSizes.map((blobSize) => {
            const row = rows.find((candidate) => candidate.transport === transport && candidate.format === format
                && candidate.blobSize === blobSize && candidate.error === 0);
            if (!row || row[metricInfo.field] <= 0) {
                return null;
            }
            return {
                row,
                value: row[metricInfo.field] * (metricInfo.scale || 1)
            };
        });

        return {
            label,
            color: colors[index % colors.length],
            points
        };
    });
}

function niceMax(value) {
    if (!Number.isFinite(value) || value <= 0) {
        return 1;
    }

    const exponent = Math.floor(Math.log10(value));
    const magnitude = Math.pow(10, exponent);
    const normalized = value / magnitude;
    if (normalized <= 2) {
        return 2 * magnitude;
    }
    if (normalized <= 5) {
        return 5 * magnitude;
    }
    return 10 * magnitude;
}

function prepareCanvas() {
    const canvas = document.getElementById("benchmarkChart");
    const rect = canvas.getBoundingClientRect();
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.floor(rect.width * ratio));
    canvas.height = Math.max(1, Math.floor(rect.height * ratio));

    const context = canvas.getContext("2d");
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    return {
        canvas,
        context,
        width: rect.width,
        height: rect.height
    };
}

function xForIndex(index, count, left, width) {
    if (count <= 1) {
        return left + width / 2;
    }
    return left + (index / (count - 1)) * width;
}

function drawChart(blobSizes, datasets, metric) {
    const { context, width, height } = prepareCanvas();
    const metricInfo = metricDefinitions[metric];
    const margin = { left: 78, right: 28, top: 28, bottom: 72 };
    const plotWidth = Math.max(1, width - margin.left - margin.right);
    const plotHeight = Math.max(1, height - margin.top - margin.bottom);
    const visibleDatasets = datasets.filter((dataset) => !hiddenSeries.has(dataset.label));
    const values = visibleDatasets.flatMap((dataset) => dataset.points.filter(Boolean).map((point) => point.value));
    const yMax = niceMax(Math.max(0, ...values) * 1.08);
    chartState.points = [];
    chartState.metric = metric;

    context.clearRect(0, 0, width, height);
    context.fillStyle = "#ffffff";
    context.fillRect(0, 0, width, height);
    context.font = "12px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif";
    context.lineWidth = 1;

    if (values.length === 0 || blobSizes.length === 0) {
        context.fillStyle = "#95a5a6";
        context.textAlign = "center";
        context.textBaseline = "middle";
        context.font = "14px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif";
        context.fillText(datasets.length === 0
            ? "No successful benchmark rows match the selected filters."
            : "All matching series are hidden. Click a legend item to show it again.",
            width / 2,
            height / 2);
        renderLegend(datasets);
        return;
    }

    const yForValue = (value) => margin.top + plotHeight - (value / yMax) * plotHeight;

    context.strokeStyle = "#e5e9ef";
    context.fillStyle = "#777777";
    context.textAlign = "right";
    context.textBaseline = "middle";
    for (let i = 0; i <= 5; ++i) {
        const value = (yMax * i) / 5;
        const y = yForValue(value);
        context.beginPath();
        context.moveTo(margin.left, y);
        context.lineTo(width - margin.right, y);
        context.stroke();
        context.fillText(formatNumber(value), margin.left - 10, y);
    }

    context.strokeStyle = "#ccd1d9";
    context.beginPath();
    context.moveTo(margin.left, margin.top);
    context.lineTo(margin.left, margin.top + plotHeight);
    context.lineTo(width - margin.right, margin.top + plotHeight);
    context.stroke();

    context.fillStyle = "#555555";
    context.textAlign = "center";
    context.textBaseline = "top";
    blobSizes.forEach((size, index) => {
        const x = xForIndex(index, blobSizes.length, margin.left, plotWidth);
        context.strokeStyle = "#eef0f2";
        context.beginPath();
        context.moveTo(x, margin.top);
        context.lineTo(x, margin.top + plotHeight);
        context.stroke();

        context.save();
        context.translate(x, margin.top + plotHeight + 18);
        if (blobSizes.length > 7) {
            context.rotate(-Math.PI / 6);
            context.textAlign = "right";
        }
        context.fillText(formatSize(size), 0, 0);
        context.restore();
    });

    context.font = "bold 12px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif";
    context.fillStyle = "#555555";
    context.textAlign = "center";
    context.textBaseline = "bottom";
    context.fillText("Blob Size (Bytes)", margin.left + plotWidth / 2, height - 8);

    context.save();
    context.translate(18, margin.top + plotHeight / 2);
    context.rotate(-Math.PI / 2);
    context.fillText(metricInfo.axis, 0, 0);
    context.restore();

    visibleDatasets.forEach((dataset) => {
        context.strokeStyle = dataset.color;
        context.lineWidth = 2;
        context.beginPath();
        let hasSegment = false;
        dataset.points.forEach((point, index) => {
            if (!point) {
                hasSegment = false;
                return;
            }

            const x = xForIndex(index, blobSizes.length, margin.left, plotWidth);
            const y = yForValue(point.value);
            if (!hasSegment) {
                context.moveTo(x, y);
                hasSegment = true;
            } else {
                context.lineTo(x, y);
            }
        });
        context.stroke();

        dataset.points.forEach((point, index) => {
            if (!point) {
                return;
            }

            const x = xForIndex(index, blobSizes.length, margin.left, plotWidth);
            const y = yForValue(point.value);
            context.fillStyle = dataset.color;
            context.strokeStyle = "#ffffff";
            context.lineWidth = 1.5;
            context.beginPath();
            context.arc(x, y, 4, 0, Math.PI * 2);
            context.fill();
            context.stroke();

            chartState.points.push({
                x,
                y,
                label: dataset.label,
                color: dataset.color,
                row: point.row,
                value: point.value
            });
        });
    });

    renderLegend(datasets);
}

function renderLegend(datasets) {
    const legend = document.getElementById("legend");
    legend.textContent = "";
    datasets.forEach((dataset) => {
        const item = document.createElement("button");
        item.className = "legend-item";
        if (hiddenSeries.has(dataset.label)) {
            item.classList.add("hidden");
        }
        item.type = "button";
        item.setAttribute("aria-pressed", hiddenSeries.has(dataset.label) ? "false" : "true");
        item.title = hiddenSeries.has(dataset.label) ? "Show this series" : "Hide this series";
        item.addEventListener("click", () => {
            if (hiddenSeries.has(dataset.label)) {
                hiddenSeries.delete(dataset.label);
            } else {
                hiddenSeries.add(dataset.label);
            }
            updateChart();
        });
        const swatch = document.createElement("span");
        swatch.className = "legend-swatch";
        swatch.style.background = dataset.color;
        item.appendChild(swatch);
        item.append(document.createTextNode(dataset.label));
        legend.appendChild(item);
    });
}

function renderTable(rows) {
    const tableBody = document.getElementById("rows");
    tableBody.textContent = "";
    rows
        .slice()
        .sort((a, b) => a.transport.localeCompare(b.transport) || a.format.localeCompare(b.format) || a.blobSize - b.blobSize)
        .forEach((row) => {
            const tableRow = document.createElement("tr");
            const values = [
                row.transport,
                row.format,
                String(row.blobSize),
                row.error === 0 ? formatNumber(row.avgUs) : "",
                row.error === 0 ? formatNumber(row.p50Us) : "",
                row.error === 0 ? formatNumber(row.p90Us) : "",
                row.error === 0 ? formatNumber(row.p95Us) : "",
                String(row.samples || 1),
                String(row.failures || 0),
                row.error === 0 ? formatNumber(row.avgUsStddev || 0) : "",
                row.error === 0 ? formatNumber(row.payloadMBps) : "",
                row.error === 0 ? formatNumber(row.roundTripMBps) : "",
                row.error === 0 ? (row.failures ? `OK (${row.failures} failed)` : "OK") : `error ${row.error}`
            ];
            values.forEach((value) => {
                const cell = document.createElement("td");
                cell.textContent = value;
                tableRow.appendChild(cell);
            });
            tableBody.appendChild(tableRow);
        });
}

function updateChart() {
    const filters = currentFilters();
    const rows = filteredRows(filters);
    const sizes = selectedBlobSizes(filters, rows);
    const datasets = buildDatasets(rows, sizes, filters.metric);
    drawChart(sizes, datasets, filters.metric);
    renderTable(rows);
}

function handlePointerMove(event) {
    const tooltip = document.getElementById("chartTooltip");
    const rect = event.currentTarget.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    let nearest = null;
    let nearestDistance = Infinity;

    chartState.points.forEach((point) => {
        const distance = Math.hypot(point.x - x, point.y - y);
        if (distance < nearestDistance) {
            nearest = point;
            nearestDistance = distance;
        }
    });

    if (!nearest || nearestDistance > 14) {
        tooltip.style.display = "none";
        return;
    }

    const metricInfo = metricDefinitions[chartState.metric];
    tooltip.innerHTML = `<strong>${escapeHtml(nearest.label)}</strong><br>`
        + `Blob: ${escapeHtml(formatSize(nearest.row.blobSize))}<br>`
        + `${escapeHtml(metricInfo.label)}: ${escapeHtml(formatNumber(nearest.value))} ${escapeHtml(metricInfo.unit)}<br>`
        + `Avg: ${escapeHtml(formatNumber(nearest.row.avgUs / 1000))} ms, p95: ${escapeHtml(formatNumber(nearest.row.p95Us / 1000))} ms<br>`
        + `Samples: ${escapeHtml(nearest.row.samples || 1)}, Avg SD: ${escapeHtml(formatNumber((nearest.row.avgUsStddev || 0) / 1000))} ms`;
    tooltip.style.left = `${Math.min(x + 16, rect.width - 320)}px`;
    tooltip.style.top = `${Math.max(8, y - 18)}px`;
    tooltip.style.display = "block";
}

function handlePointerLeave() {
    document.getElementById("chartTooltip").style.display = "none";
}

function init() {
    populateFilters();
    ["transportFilter", "formatFilter", "sizeFilter", "metricFilter"].forEach((id) => {
        document.getElementById(id).addEventListener("change", updateChart);
    });
    document.getElementById("benchmarkChart").addEventListener("mousemove", handlePointerMove);
    document.getElementById("benchmarkChart").addEventListener("mouseleave", handlePointerLeave);
    window.addEventListener("resize", updateChart);
    updateChart();
}

init();
</script>
</body>
</html>
)HTML";

        output.flush();
        if (!output)
        {
            fmt::print(stderr, "Failed while writing benchmark report: {}\n", report_path.string());
            return false;
        }

        return true;
    }
} // namespace

int main(
    int argc,
    char** argv)
{
    using namespace comprehensive::v1;

    tune_allocator_for_large_payload_benchmarks();

    benchmark_filters filters;
    const auto status = parse_filters(argc, argv, filters);
    if (status == parse_status::help)
        return 0;
    if (status == parse_status::error)
        return 1;

    const std::vector<encoding_info> encodings = {
        {rpc::encoding::yas_binary, "yas_binary"},
        {rpc::encoding::yas_compressed_binary, "yas_compressed"},
#ifdef CANOPY_BUILD_NANOPB
        {rpc::encoding::nanopb, "nanopb"},
#endif
#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
        {rpc::encoding::protocol_buffers, "protocol_buffers"},
#endif
    };

    const std::vector<size_t> blob_sizes = {
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

    std::vector<benchmark_job> jobs;
#ifdef CANOPY_BUILD_COROUTINE
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "local",
        [](rpc::encoding enc, size_t blob_size)
        {
            auto scheduler = make_benchmark_scheduler();
            return coro::sync_wait(run_local_benchmark(scheduler, enc, blob_size));
        });
#else
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "local",
        [](rpc::encoding enc, size_t blob_size) { return run_local_benchmark(enc, blob_size); });
#endif

#ifndef CANOPY_BUILD_COROUTINE
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "blocking_dll",
        [](rpc::encoding enc, size_t blob_size) { return run_blocking_dll_benchmark(enc, blob_size); });
#else
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "unshared_scheduler_dll",
        [](rpc::encoding enc, size_t blob_size)
        {
            auto scheduler = make_benchmark_scheduler();
            return coro::sync_wait(run_unshared_scheduler_dll_benchmark(scheduler, enc, blob_size));
        });

    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "shared_scheduler_dll",
        [](rpc::encoding enc, size_t blob_size)
        {
            auto scheduler = make_benchmark_scheduler();
            return coro::sync_wait(run_shared_scheduler_dll_benchmark(scheduler, enc, blob_size));
        });

    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "ipc_direct",
        [](rpc::encoding enc, size_t blob_size)
        {
            auto scheduler = make_benchmark_scheduler();
            return coro::sync_wait(run_ipc_direct_benchmark(scheduler, enc, blob_size));
        });

    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "ipc_dll",
        [](rpc::encoding enc, size_t blob_size)
        {
            auto scheduler = make_benchmark_scheduler();
            return coro::sync_wait(run_ipc_dll_benchmark(scheduler, enc, blob_size));
        });

    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "spsc",
        [](rpc::encoding enc, size_t blob_size) { return run_spsc_benchmark(enc, blob_size); });

#endif
#ifndef CANOPY_BUILD_COROUTINE
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "tcp_blocking",
        [](rpc::encoding enc, size_t blob_size)
        { return run_tcp_blocking_benchmark(enc, blob_size, allocate_loopback_port()); });

#  ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "tls+tcp_blocking",
        [](rpc::encoding enc, size_t blob_size)
        { return run_tls_tcp_blocking_benchmark(enc, blob_size, allocate_loopback_port()); });
#  endif

#  ifdef CANOPY_BUILD_WEBSOCKET
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "ws+tcp_blocking",
        [](rpc::encoding enc, size_t blob_size)
        { return run_websocket_tcp_blocking_benchmark(enc, blob_size, allocate_loopback_port()); });
#    ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
    add_transport_jobs(
        jobs,
        filters,
        encodings,
        blob_sizes,
        "tls+ws+tcp_blocking",
        [](rpc::encoding enc, size_t blob_size)
        { return run_tls_websocket_tcp_blocking_benchmark(enc, blob_size, allocate_loopback_port()); });
#    endif
#  endif
#endif

#ifdef CANOPY_BUILD_COROUTINE
    const std::vector<io_uring_benchmark_variant> tcp_coroutine_variants = {
        {"tcp_coroutine_proactor_4k", true, 4096},
        {"tcp_coroutine_proactor_64k", true, 65536},
        {"tcp_coroutine_cooperative_4k", false, 4096},
        {"tcp_coroutine_cooperative_64k", false, 65536},
    };

    for (const auto& variant : tcp_coroutine_variants)
    {
        if (!should_run_tcp_coroutine_variant(filters, variant.name))
            continue;

        add_selected_transport_jobs(
            jobs,
            filters,
            encodings,
            blob_sizes,
            variant.name,
            [use_proactor = variant.use_proactor, host_buffer_size = variant.host_buffer_size](
                rpc::encoding enc, size_t blob_size)
            {
                return run_tcp_coroutine_benchmark(
                    enc, blob_size, allocate_loopback_port(), use_proactor, host_buffer_size);
            });
    }

#  ifdef CANOPY_BENCHMARK_SGX_COROUTINE
    const std::vector<io_uring_benchmark_variant> sgx_tcp_coroutine_variants = {
        {"sgx_io_uring_proactor_4k", true, 4096},
        {"sgx_io_uring_proactor_64k", true, 65536},
        {"sgx_io_uring_cooperative_4k", false, 4096},
        {"sgx_io_uring_cooperative_64k", false, 65536},
    };

    for (const auto& variant : sgx_tcp_coroutine_variants)
    {
        if (!should_run_sgx_io_uring_variant(filters, variant.name))
            continue;

        add_selected_transport_jobs(
            jobs,
            filters,
            encodings,
            blob_sizes,
            variant.name,
            [use_proactor = variant.use_proactor, host_buffer_size = variant.host_buffer_size](
                rpc::encoding enc, size_t blob_size)
            { return run_sgx_coroutine_io_uring_benchmark(enc, blob_size, use_proactor, host_buffer_size); });
    }

    const std::vector<io_uring_benchmark_variant> sgx_io_uring_pair_variants = {
        {"sgx_io_uring_pair_proactor_4k", true, 4096},
        {"sgx_io_uring_pair_proactor_64k", true, 65536},
        {"sgx_io_uring_pair_cooperative_4k", false, 4096},
        {"sgx_io_uring_pair_cooperative_64k", false, 65536},
    };

    for (const auto& variant : sgx_io_uring_pair_variants)
    {
        if (!should_run_sgx_io_uring_pair_variant(filters, variant.name))
            continue;

        add_selected_transport_jobs(
            jobs,
            filters,
            encodings,
            blob_sizes,
            variant.name,
            [use_proactor = variant.use_proactor, host_buffer_size = variant.host_buffer_size](
                rpc::encoding enc, size_t blob_size)
            { return run_sgx_coroutine_io_uring_pair_benchmark(enc, blob_size, use_proactor, host_buffer_size); });
    }
#  endif
#endif

    if (jobs.empty())
    {
        fmt::print(stderr, "No benchmark entries matched the selected filters.\n");
        print_usage();
        return 1;
    }

    fmt::print("Canopy Comprehensive Demo - Benchmark\n");
    fmt::print("====================================\n\n");
    fmt::print(
        "Measurement passes: {}, row order: {}, shuffle seed: {}\n",
        filters.passes,
        filters.shuffle ? "shuffled" : "ordered",
        filters.shuffle_seed);
    if (filters.passes > 1)
    {
        fmt::print(
            "Aggregate rows use the median of successful pass-level statistics; Avg SD reports cross-pass average "
            "latency deviation.\n\n");
    }

    if (filters.passes == 1)
        print_header();

    std::vector<benchmark_report_row> sample_rows;
    const auto executions = make_execution_order(jobs.size(), filters);
    for (size_t index = 0; index < executions.size(); ++index)
    {
        const auto execution = executions[index];
        const auto& job = jobs[execution.job_index];
        if (filters.passes > 1)
        {
            fmt::print(
                "sample {}/{} pass {}/{} {} | {} | {}\n",
                index + 1,
                executions.size(),
                execution.pass + 1,
                filters.passes,
                job.transport,
                job.format,
                job.blob_size);
        }

        const auto result = job.run();
        record_benchmark_result(
            sample_rows, job.transport.c_str(), job.format.c_str(), job.blob_size, result, filters.passes == 1);
    }

    auto report_rows = filters.passes == 1 ? sample_rows : aggregate_report_rows(sample_rows);
    if (filters.passes > 1)
    {
        fmt::print("\nAggregate benchmark results\n");
        print_header();
        for (const auto& row : report_rows)
            print_report_row(row);

        print_footer();
        fmt::print("Aggregate quality summary\n");
        for (const auto& row : report_rows)
            print_quality_row(row);
    }
    else
    {
        print_footer();
    }

    if (filters.write_html_report)
    {
        if (!write_html_report(filters.html_report_path, report_rows))
            return 1;
        fmt::print("HTML benchmark report written to {}\n", filters.html_report_path.string());
    }

    return 0;
}
