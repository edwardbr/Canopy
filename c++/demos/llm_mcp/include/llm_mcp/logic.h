/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>

#include <llm_mcp/llm_mcp.h>
#include <rpc/rpc.h>

namespace llm_mcp
{
    namespace v1
    {
        class schema_mcp final : public rpc::base<schema_mcp, i_mcp>
        {
            std::string target_name_;
            rpc::shared_ptr<rpc::casting_interface> target_;

        public:
            schema_mcp(
                std::string target_name,
                rpc::shared_ptr<rpc::casting_interface> target);

            CORO_TASK(llm_mcp_error) list_interfaces(std::vector<rpc::interface_descriptor>& interfaces) override;
            CORO_TASK(llm_mcp_error) list_tools(std::vector<mcp_tool>& tools) override;
            CORO_TASK(llm_mcp_error)
            call_tool(
                const std::string& tool_name,
                const std::string& arguments_json,
                std::string& result_json) override;
        };

        [[nodiscard]] rpc::shared_ptr<i_mcp> make_schema_mcp(
            std::string target_name,
            rpc::shared_ptr<rpc::casting_interface> target);

        [[nodiscard]] rpc::shared_ptr<i_llm_factory> make_demo_llm_factory();
        [[nodiscard]] rpc::shared_ptr<i_llm_factory> make_llama_cpp_llm_factory(std::string model_path);
        [[nodiscard]] rpc::shared_ptr<i_llm_factory> make_environment_llm_factory();
    }
}
