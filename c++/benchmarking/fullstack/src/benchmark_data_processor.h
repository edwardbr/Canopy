/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <comprehensive/comprehensive.h>

namespace comprehensive::v1
{
    class benchmark_data_processor_impl : public rpc::base<benchmark_data_processor_impl, i_data_processor>
    {
    public:
        CORO_TASK(int)
        process_vector(
            const std::vector<int>& input,
            std::vector<int>& output) override
        {
            output.clear();
            for (int value : input)
                output.push_back(value * 2);
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        process_map(
            const std::map<
                std::string,
                int>& input,
            std::map<
                std::string,
                int>& output) override
        {
            output.clear();
            for (const auto& [key, value] : input)
                output[key] = value * 2;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        process_struct(
            const std::string& input,
            std::string& output) override
        {
            output = "Processed: " + input;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        echo_binary(
            const std::vector<uint8_t>& data,
            std::vector<uint8_t>& response) override
        {
            response = data;
            CO_RETURN rpc::error::OK();
        }
    };

    inline rpc::shared_ptr<i_data_processor> make_benchmark_data_processor()
    {
        return rpc::shared_ptr<i_data_processor>(new benchmark_data_processor_impl());
    }
}
