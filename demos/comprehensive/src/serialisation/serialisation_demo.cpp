/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Serialization Demo
 *   Demonstrates all supported serialization formats
 *
 *   Concept: RPC++ supports multiple serialization formats:
 *   - yas_binary: High-performance binary format (default)
 *   - yas_compressed_binary: Binary with compression
 *   - yas_json: Human-readable JSON
 *   - protocol_buffers: Google's protobuf format
 *
 *   MISCONCEPTION REPORT:
 *   ---------------------
 *   1. The serialization format is negotiated between proxy and stub.
 *      If a format is not supported, fallback to yas_json occurs.
 *
 *   2. Protocol Buffers support requires protobuf to be enabled in the build
 *      and the IDL to be configured with 'protocol_buffers' in the format list.
 *
 *   3. Compression (yas_compressed_binary) has overhead - use only for
 *      large payloads where compression ratio justifies CPU cost.
 *
 *   4. All formats are interoperable - the same data can be serialized
 *      in one format and deserialized in another (with some exceptions
 *      for format-specific features like compression).
 *
 *   To build and run:
 *   1. cmake --preset Debug (or Coroutine_Debug)
 *   2. cmake --build build --target serialisation_demo
 *   3. ./build/output/debug/demos/comprehensive/serialisation_demo
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <map>
#include <string>

namespace comprehensive
{
    namespace v1
    {

        void print_separator(const std::string& title)
        {
            std::cout << "\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "  " << title << "\n";
            std::cout << std::string(60, '=') << "\n";
        }

        void print_result(const std::string& operation, int error, const std::string& details = "")
        {
            RPC_INFO("{}: ", operation);
            if (error == rpc::error::OK())
            {
                RPC_INFO("OK");
            }
            else
            {
                RPC_INFO("ERROR {}", static_cast<int>(error));
            }
            if (!details.empty())
            {
                RPC_INFO(" ({})", details);
            }
        }

#ifdef CANOPY_BUILD_COROUTINE
        CORO_TASK(bool) run_serialisation_demo(std::shared_ptr<coro::io_scheduler> scheduler)
#else
        bool run_serialisation_demo()
#endif
        {
            print_separator("SERIALIZATION DEMO");

            // Create data processor for testing
            auto processor = create_data_processor();

            RPC_INFO("");
            RPC_INFO("Testing various data types and serialization:");

            // Test 1: Vector processing
            {
                print_separator("1. VECTOR SERIALIZATION");
                std::vector<int> input{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
                std::vector<int> output;

                auto error = CO_AWAIT processor->process_vector(input, output);
                print_result("process_vector", error);

                if (error == rpc::error::OK())
                {
                    RPC_INFO("  Input:  {{");
                    for (size_t i = 0; i < input.size(); ++i)
                    {
                        RPC_INFO("{}", input[i]);
                        if (i < input.size() - 1)
                        {
                            RPC_INFO(", ");
                        }
                    }
                    RPC_INFO("}}");
                    RPC_INFO("  Output: {{");
                    for (size_t i = 0; i < output.size(); ++i)
                    {
                        RPC_INFO("{}", output[i]);
                        if (i < output.size() - 1)
                        {
                            RPC_INFO(", ");
                        }
                    }
                    RPC_INFO("}}");
                }
            }

            // Test 2: Map processing
            {
                print_separator("2. MAP SERIALIZATION");
                std::map<std::string, int> input{{"a", 1}, {"b", 2}, {"c", 3}};
                std::map<std::string, int> output;

                auto error = CO_AWAIT processor->process_map(input, output);
                print_result("process_map", error);

                if (error == rpc::error::OK())
                {
                    RPC_INFO("  Input:  {{a:1, b:2, c:3}}");
                    RPC_INFO("  Output: {{");
                    bool first = true;
                    for (const auto& [key, val] : output)
                    {
                        if (!first)
                        {
                            RPC_INFO(", ");
                        }
                        RPC_INFO("{}:{}", key, val);
                        first = false;
                    }
                    RPC_INFO("}}");
                }
            }

            // Test 3: Complex struct (simplified to string for demo)
            {
                print_separator("3. STRUCT SERIALIZATION");
                std::string input = "test_data_bundle_42";
                std::string output;

                auto error = CO_AWAIT processor->process_struct(input, output);
                print_result("process_struct", error);

                if (error == rpc::error::OK())
                {
                    RPC_INFO("  Input:  {}", input);
                    RPC_INFO("  Output: {}", output);
                }
            }

            // Test 4: Binary data
            {
                print_separator("4. BINARY DATA SERIALIZATION");
                std::vector<uint8_t> input(1024);
                for (size_t i = 0; i < input.size(); ++i)
                {
                    input[i] = static_cast<uint8_t>(i % 256);
                }

                std::vector<uint8_t> output;
                auto error = CO_AWAIT processor->echo_binary(input, output);
                print_result("echo_binary (1KB)", error);

                if (error == rpc::error::OK())
                {
                    RPC_INFO("  Input size:  {} bytes", input.size());
                    RPC_INFO("  Output size: {} bytes", output.size());
                    RPC_INFO("  Data match:  {}", (input == output ? "YES" : "NO"));
                }
            }

            // Test 5: Large binary data
            {
                print_separator("5. LARGE DATA SERIALIZATION (1MB)");
                std::vector<uint8_t> input(1024 * 1024);
                for (size_t i = 0; i < input.size(); ++i)
                {
                    input[i] = static_cast<uint8_t>(i % 256);
                }

                std::vector<uint8_t> output;
                auto error = CO_AWAIT processor->echo_binary(input, output);
                print_result("echo_binary (1MB)", error);

                if (error == rpc::error::OK())
                {
                    RPC_INFO("  Input size:  {} bytes", input.size());
                    RPC_INFO("  Output size: {} bytes", output.size());
                    RPC_INFO("  Data match:  {}", (input == output ? "YES" : "NO"));
                }
            }

            // Test 6: Direct serialization API
            {
                print_separator("6. DIRECT SERIALIZATION API");

                RPC_INFO("  Note: Direct serialization API available in serialiser.h");
                RPC_INFO("        #include <rpc/internal/serialiser.h>");
                RPC_INFO("        rpc::serialise<T, encoding>(obj)");
                RPC_INFO("        rpc::deserialise<encoding>(data, obj)");
                RPC_INFO("  This requires types to be registered with YAS serializer");
            }

            print_separator("SERIALIZATION DEMO COMPLETED");
            CO_RETURN true;
        }

        CORO_TASK(void)
        demo_task(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::io_scheduler> scheduler,
#endif
            bool* result_flag,
            bool* completed_flag)
        {
            bool res = CO_AWAIT run_serialisation_demo(
#ifdef CANOPY_BUILD_COROUTINE
                scheduler
#endif
            );
            *result_flag = res;
            *completed_flag = true;
            CO_RETURN;
        }
    }
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        std::cout << "[DEBUG] " << message << std::endl;
        break;
    case 1:
        std::cout << "[TRACE] " << message << std::endl;
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

int main()
{
    RPC_INFO("RPC++ Comprehensive Demo - Serialization");
    RPC_INFO("========================================");

#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{
            .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{
                .thread_count = std::thread::hardware_concurrency(),
            },
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool
        });

    bool result = false;
    bool completed = false;

    scheduler->spawn(comprehensive::v1::demo_task(scheduler, &result, &completed));

    while (!completed)
    {
        scheduler->process_events(std::chrono::milliseconds(1));
    }

    return result ? 0 : 1;
#else
    return comprehensive::v1::run_serialisation_demo() ? 0 : 1;
#endif
}
