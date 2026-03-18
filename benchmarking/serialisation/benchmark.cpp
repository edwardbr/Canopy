/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Serialisation Benchmark
 *   Measures serialise + deserialise round-trip throughput for every type
 *   covered by the serialiser unit tests, across all four encoding formats:
 *     - yas_binary
 *     - yas_compressed_binary
 *     - yas_json
 *     - protocol_buffers
 *
 *   Types exercised (matching serialiser_test.cpp):
 *     Scalars : int8, uint8, int16, uint16, int32, uint32, int64, uint64,
 *               int128, uint128, float, double, std::string
 *     Vectors : vector<T> for every scalar type above
 *     Arrays  : array<T,4> for every scalar type above
 *     Structs : something_complicated, something_more_complicated,
 *               test_template<int>, something_with_a_template
 *
 *   Statistics: middle 80% of 10 000 round-trips (drop first/last 10%).
 *
 *   To build and run:
 *     cmake --preset Debug -DCANOPY_BUILD_BENCHMARKING=ON
 *     cmake --build build_debug --target serialisation_benchmark
 *     ./build_debug/output/serialisation_benchmark
 */

#include <serialiser_test/test_types.h>

#include <rpc/rpc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace serialisation_benchmark
{
    using clock_type = std::chrono::steady_clock;

    constexpr size_t call_count = 10000;
    constexpr size_t trim_each_side = call_count / 10;

    // -------------------------------------------------------------------------
    // Statistics helpers (identical pattern to fullstack benchmark)
    // -------------------------------------------------------------------------

    struct benchmark_stats
    {
        double avg_ns = 0.0;
        double min_ns = 0.0;
        double max_ns = 0.0;
        double p50_ns = 0.0;
        double p90_ns = 0.0;
        double p95_ns = 0.0;
    };

    benchmark_stats compute_stats(std::vector<int64_t> samples)
    {
        benchmark_stats stats{};
        if (samples.size() < (trim_each_side * 2))
            return stats;

        std::sort(samples.begin(), samples.end());

        const size_t begin = trim_each_side;
        const size_t end = samples.size() - trim_each_side;
        std::vector<int64_t> mid(samples.begin() + static_cast<long>(begin), samples.begin() + static_cast<long>(end));

        const size_t n = mid.size();
        const auto sum = std::accumulate(mid.begin(), mid.end(), int64_t{0});
        stats.avg_ns = static_cast<double>(sum) / static_cast<double>(n);
        stats.min_ns = static_cast<double>(mid.front());
        stats.max_ns = static_cast<double>(mid.back());
        stats.p50_ns = static_cast<double>(mid[(n * 50) / 100]);
        stats.p90_ns = static_cast<double>(mid[(n * 90) / 100]);
        stats.p95_ns = static_cast<double>(mid[(n * 95) / 100]);
        return stats;
    }

    struct encoding_info
    {
        rpc::encoding enc;
        const char* name;

        template<typename T> [[nodiscard]] std::vector<uint8_t> serialise(const T& obj) const
        {
            return rpc::serialise(obj, enc);
        }

        template<typename T> std::string deserialise(const rpc::byte_span& data, T& obj) const
        {
            return rpc::deserialise(enc, data, obj);
        }
    };

    static const std::vector<encoding_info> all_encodings = {
        {rpc::encoding::yas_binary, "yas_binary"},
        {rpc::encoding::yas_compressed_binary, "yas_compressed"},
        {rpc::encoding::yas_json, "yas_json"},
        {rpc::encoding::protocol_buffers, "protocol_buffers"},
    };

    // -------------------------------------------------------------------------
    // Core measurement loop: round-trip serialize+deserialize call_count times
    // -------------------------------------------------------------------------

    template<typename T> benchmark_stats measure(const encoding_info& enc, const T& value)
    {
        std::vector<int64_t> samples;
        samples.reserve(call_count);

        T scratch{};

        // Warmup — not included in timing
        for (size_t i = 0; i < 100; ++i)
        {
            auto buf = enc.serialise(value);
            rpc::byte_span span(buf);
            enc.deserialise(span, scratch);
        }

        for (size_t i = 0; i < call_count; ++i)
        {
            const auto t0 = clock_type::now();
            auto buf = enc.serialise(value);
            rpc::byte_span span(buf);
            enc.deserialise(span, scratch);
            const auto t1 = clock_type::now();

            samples.push_back(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }

        return compute_stats(std::move(samples));
    }

    // -------------------------------------------------------------------------
    // Reporting
    // -------------------------------------------------------------------------

    void print_row(const char* type_name, const char* enc_name, size_t serialised_bytes, const benchmark_stats& stats)
    {
        fmt::print("{:<36} | {:<18} | {:>8} B | avg {:>8.1f} ns | p50 {:>8.1f} | p90 {:>8.1f} | p95 {:>8.1f} | min "
                   "{:>8.1f} | max {:>9.1f}\n",
            type_name,
            enc_name,
            serialised_bytes,
            stats.avg_ns,
            stats.p50_ns,
            stats.p90_ns,
            stats.p95_ns,
            stats.min_ns,
            stats.max_ns);
    }

    void print_header()
    {
        fmt::print("Serialisation Benchmark — {} round-trips, middle 80% (drop first/last 10%)\n", call_count);
        fmt::print("Warmup: 100 calls (not included in timing)\n");
        fmt::print(
            "{:-<37}+{:-<20}+{:-<11}+{:-<14}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "", "");
        fmt::print("{:<36} | {:<18} | {:>10} | {:<13} | {:<11} | {:<11} | {:<11} | {:<11} | {:<11}\n",
            "type",
            "encoding",
            "wire bytes",
            "avg",
            "p50",
            "p90",
            "p95",
            "min",
            "max");
        fmt::print(
            "{:-<37}+{:-<20}+{:-<11}+{:-<14}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "", "");
    }

    void print_footer()
    {
        fmt::print(
            "{:-<37}+{:-<20}+{:-<11}+{:-<14}+{:-<12}+{:-<12}+{:-<12}+{:-<12}+{:-<13}\n", "", "", "", "", "", "", "", "", "");
    }

    // -------------------------------------------------------------------------
    // Benchmark a single value across all encodings
    // -------------------------------------------------------------------------

    template<typename T> void bench_type(const char* type_name, const T& value)
    {
        for (const auto& enc : all_encodings)
        {
            auto buf = enc.serialise(value);
            const auto stats = measure(enc, value);
            print_row(type_name, enc.name, buf.size(), stats);
        }
    }

    // -------------------------------------------------------------------------
    // Build representative test values (same as serialiser_test.cpp)
    // -------------------------------------------------------------------------

    void run_all()
    {
        // ---- Scalar holders ----
        bench_type("int8_holder (zero)", scalar_test::int8_holder{0});
        bench_type("int8_holder (max)", scalar_test::int8_holder{std::numeric_limits<int8_t>::max()});
        bench_type("uint8_holder (max)", scalar_test::uint8_holder{std::numeric_limits<uint8_t>::max()});
        bench_type("int16_holder (max)", scalar_test::int16_holder{std::numeric_limits<int16_t>::max()});
        bench_type("uint16_holder (max)", scalar_test::uint16_holder{std::numeric_limits<uint16_t>::max()});
        bench_type("int32_holder (typical)", scalar_test::int32_holder{123456789});
        bench_type("int32_holder (max)", scalar_test::int32_holder{std::numeric_limits<int32_t>::max()});
        bench_type("uint32_holder (max)", scalar_test::uint32_holder{std::numeric_limits<uint32_t>::max()});
        bench_type("int64_holder (typical)", scalar_test::int64_holder{-9876543210LL});
        bench_type("int64_holder (max)", scalar_test::int64_holder{std::numeric_limits<int64_t>::max()});
        bench_type("uint64_holder (max)", scalar_test::uint64_holder{std::numeric_limits<uint64_t>::max()});
        bench_type("int128_holder (positive)", scalar_test::int128_holder{__int128{1} << 100});
        bench_type("uint128_holder (typical)",
            scalar_test::uint128_holder{
                (static_cast<unsigned __int128>(0xDEADBEEFCAFEBABEULL) << 64) | 0x0123456789ABCDEFULL});
        bench_type("float_holder (typical)", scalar_test::float_holder{3.14159265f});
        bench_type("double_holder (typical)", scalar_test::double_holder{3.14159265358979323846});
        bench_type("string_holder (empty)", scalar_test::string_holder{""});
        bench_type("string_holder (13 chars)", scalar_test::string_holder{"hello, world!"});

        // ---- Vector holders ----
        bench_type("int8_vec_holder (5 elems)",
            scalar_test::int8_vec_holder{
                {std::numeric_limits<int8_t>::min(), -1, 0, 1, std::numeric_limits<int8_t>::max()}});
        bench_type("uint8_vec_holder (4 elems)",
            scalar_test::uint8_vec_holder{{0, 1, 127, std::numeric_limits<uint8_t>::max()}});
        bench_type("int16_vec_holder (4 elems)",
            scalar_test::int16_vec_holder{
                {std::numeric_limits<int16_t>::min(), -1, 0, std::numeric_limits<int16_t>::max()}});
        bench_type("uint16_vec_holder (4 elems)",
            scalar_test::uint16_vec_holder{{0, 1, 1000, std::numeric_limits<uint16_t>::max()}});
        bench_type("int32_vec_holder (4 elems)",
            scalar_test::int32_vec_holder{
                {std::numeric_limits<int32_t>::min(), -1, 0, std::numeric_limits<int32_t>::max()}});
        bench_type("uint32_vec_holder (4 elems)",
            scalar_test::uint32_vec_holder{{0, 1, 123456, std::numeric_limits<uint32_t>::max()}});
        bench_type("int64_vec_holder (4 elems)",
            scalar_test::int64_vec_holder{
                {std::numeric_limits<int64_t>::min(), -1LL, 0LL, std::numeric_limits<int64_t>::max()}});
        bench_type("uint64_vec_holder (4 elems)",
            scalar_test::uint64_vec_holder{{0ULL, 1ULL, 42ULL, std::numeric_limits<uint64_t>::max()}});
        {
            scalar_test::int128_vec_holder h;
            h.value = {__int128{0},
                -(__int128{1} << 100),
                __int128{1} << 100,
                static_cast<__int128>(static_cast<unsigned __int128>(-1) >> 1)};
            bench_type("int128_vec_holder (4 elems)", h);
        }
        {
            scalar_test::uint128_vec_holder h;
            h.value = {static_cast<unsigned __int128>(0),
                (static_cast<unsigned __int128>(0xDEADBEEFULL) << 64) | 0xCAFEBABEULL,
                static_cast<unsigned __int128>(-1)};
            bench_type("uint128_vec_holder (3 elems)", h);
        }
        bench_type("float_vec_holder (4 elems)",
            scalar_test::float_vec_holder{
                {std::numeric_limits<float>::lowest(), -1.0f, 0.0f, std::numeric_limits<float>::max()}});
        bench_type("double_vec_holder (4 elems)",
            scalar_test::double_vec_holder{
                {std::numeric_limits<double>::lowest(), -1.0, 0.0, std::numeric_limits<double>::max()}});
        bench_type("string_vec_holder (4 elems)", scalar_test::string_vec_holder{{"", "hello", "world", "test"}});

        // ---- Array holders (size 4) ----
        bench_type("int8_arr_holder",
            scalar_test::int8_arr_holder{{{std::numeric_limits<int8_t>::min(), -1, 0, std::numeric_limits<int8_t>::max()}}});
        bench_type("uint8_arr_holder", scalar_test::uint8_arr_holder{{{0, 1, 127, std::numeric_limits<uint8_t>::max()}}});
        bench_type("int16_arr_holder",
            scalar_test::int16_arr_holder{
                {{std::numeric_limits<int16_t>::min(), -1, 0, std::numeric_limits<int16_t>::max()}}});
        bench_type(
            "uint16_arr_holder", scalar_test::uint16_arr_holder{{{0, 1, 1000, std::numeric_limits<uint16_t>::max()}}});
        bench_type("int32_arr_holder",
            scalar_test::int32_arr_holder{
                {{std::numeric_limits<int32_t>::min(), -1, 0, std::numeric_limits<int32_t>::max()}}});
        bench_type("uint32_arr_holder",
            scalar_test::uint32_arr_holder{{{0, 1, 123456, std::numeric_limits<uint32_t>::max()}}});
        bench_type("int64_arr_holder",
            scalar_test::int64_arr_holder{
                {{std::numeric_limits<int64_t>::min(), -1LL, 0LL, std::numeric_limits<int64_t>::max()}}});
        bench_type("uint64_arr_holder",
            scalar_test::uint64_arr_holder{{{0ULL, 1ULL, 42ULL, std::numeric_limits<uint64_t>::max()}}});
        {
            scalar_test::int128_arr_holder h;
            h.value = {__int128{0},
                -(__int128{1} << 100),
                __int128{1} << 100,
                static_cast<__int128>(static_cast<unsigned __int128>(-1) >> 1)};
            bench_type("int128_arr_holder", h);
        }
        {
            scalar_test::uint128_arr_holder h;
            h.value = {static_cast<unsigned __int128>(0),
                (static_cast<unsigned __int128>(0xDEADBEEFULL) << 64) | 0xCAFEBABEULL,
                static_cast<unsigned __int128>(-1),
                static_cast<unsigned __int128>(1) << 64};
            bench_type("uint128_arr_holder", h);
        }
        bench_type("float_arr_holder",
            scalar_test::float_arr_holder{
                {{std::numeric_limits<float>::lowest(), -1.0f, 0.0f, std::numeric_limits<float>::max()}}});
        bench_type("double_arr_holder",
            scalar_test::double_arr_holder{
                {{std::numeric_limits<double>::lowest(), -1.0, 0.0, std::numeric_limits<double>::max()}}});
        bench_type("string_arr_holder", scalar_test::string_arr_holder{{{"", "hello", "world", "test"}}});

        // ---- Composite structs ----
        {
            scalar_test::something_complicated obj;
            obj.int_val = 123456789;
            obj.string_val = "benchmark_string";
            bench_type("something_complicated", obj);
        }
        {
            scalar_test::something_more_complicated obj;
            obj.vector_val.push_back({1, "first"});
            obj.vector_val.push_back({2, "second"});
            obj.vector_val.push_back({3, "third"});
            obj.map_val["key1"] = {FLD(int_val) 10, FLD(string_val) "map_first"};
            obj.map_val["key2"] = {FLD(int_val) 20, FLD(string_val) "map_second"};
            bench_type("something_more_complicated", obj);
        }
        {
            scalar_test::test_template<int> obj;
            obj.type_t = 42;
            bench_type("test_template<int>", obj);
        }
        {
            scalar_test::something_with_a_template obj;
            obj.template_int_val.type_t = 99;
            bench_type("something_with_a_template", obj);
        }
    }
}

int main()
{
    std::cout << "RPC++ Serialisation Benchmark\n";
    std::cout << "=============================\n\n";

    serialisation_benchmark::print_header();
    serialisation_benchmark::run_all();
    serialisation_benchmark::print_footer();

    return 0;
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        std::cout << "[TRACE] " << message << std::endl;
        break;
    case 1:
        std::cout << "[DEBUG] " << message << std::endl;
        break;
    case 2:
        std::cout << "[INFO] " << message << std::endl;
        break;
    case 3:
        std::cout << "[WARN] " << message << std::endl;
        break;
    case 4:
        std::cout << "[ERROR] " << message << std::endl;
        break;
    case 5:
        std::cout << "[CRITICAL] " << message << std::endl;
        break;
    default:
        std::cout << "[LOG " << level << "] " << message << std::endl;
        break;
    }
}
