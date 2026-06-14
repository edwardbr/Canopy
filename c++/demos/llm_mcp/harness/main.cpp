/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <iostream>
#include <string>
#include <vector>

#include <llm_mcp/logic.h>
#include <rpc_objects/calculator/calculator_impl.h>
#include <rpc_objects/text_tools/text_tools_impl.h>
#include <rpc/rpc.h>

namespace llm = llm_mcp::v1;
namespace calc = calculator::v1;
namespace text = text_tools::v1;

namespace
{
    class event_sink final : public rpc::base<event_sink, llm::i_llm_events>
    {
    public:
        std::vector<std::string> pieces;
        bool completed = false;

        CORO_TASK(llm::llm_mcp_error) piece(const std::string& value) override
        {
            pieces.push_back(value);
            std::cout << value << "\n";
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(llm::llm_mcp_error) complete() override
        {
            completed = true;
            std::cout << "complete\n";
            CO_RETURN rpc::error::OK();
        }
    };

    [[nodiscard]] bool contains(
        const std::string& haystack,
        const std::string& needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    [[nodiscard]] bool any_piece_contains(
        const event_sink& sink,
        const std::string& needle)
    {
        for (const auto& piece : sink.pieces)
        {
            if (contains(piece, needle))
                return true;
        }
        return false;
    }

    void print_tools(const std::vector<llm::mcp_tool>& tools)
    {
        std::cout << "tools:\n";
        for (const auto& tool : tools)
        {
            std::cout << "  " << tool.name << " -> " << tool.description << "\n";
            std::cout << "    input schema: " << tool.input_schema << "\n";
        }
    }

    CORO_TASK(bool) run_harness()
    {
        auto calculator = calc::make_calculator();
        auto text_tools = text::make_text_tools();

        auto calculator_mcp
            = llm::make_schema_mcp("calculator", rpc::static_pointer_cast<rpc::casting_interface>(calculator));
        auto text_mcp = llm::make_schema_mcp("text", rpc::static_pointer_cast<rpc::casting_interface>(text_tools));

        std::vector<llm::mcp_tool> calculator_tools;
        auto err = CO_AWAIT calculator_mcp->list_tools(calculator_tools);
        if (err != rpc::error::OK())
        {
            std::cout << "failed to list calculator tools: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }
        print_tools(calculator_tools);

        std::string direct_result;
        err = CO_AWAIT calculator_mcp->call_tool("calculator.add", "{\"a\":40,\"b\":2}", direct_result);
        if (err != rpc::error::OK() || !contains(direct_result, "42"))
        {
            std::cout << "direct calculator.add JSON call failed: error=" << static_cast<int>(err)
                      << " result=" << direct_result << "\n";
            CO_RETURN false;
        }
        std::cout << "direct calculator.add result: " << direct_result << "\n";

        auto factory = llm::make_environment_llm_factory();
        if (!factory)
        {
            std::cout << "real LLM backend requested but unavailable in this build\n";
            CO_RETURN false;
        }
        rpc::shared_ptr<llm::i_llm> model;
        llm::llm_options options;
        options.system_prompt = "Use MCP schemas to call Canopy RPC tools with generated yas_json.";
        options.temperature = 0.2;
        options.min_p = 0.05;
        options.stream_events = true;
        options.max_tool_calls = 4;

        err = CO_AWAIT factory->create_llm(options, model);
        if (err != rpc::error::OK() || !model)
        {
            std::cout << "failed to create demo LLM: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }

        auto sink = rpc::shared_ptr<event_sink>(new event_sink());
        err = CO_AWAIT model->set_callback(rpc::static_pointer_cast<llm::i_llm_events>(sink));
        if (err != rpc::error::OK())
        {
            std::cout << "failed to set callback: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }

        err = CO_AWAIT model->add_mcp(calculator_mcp);
        if (err != rpc::error::OK())
        {
            std::cout << "failed to add calculator MCP: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }

        err = CO_AWAIT model->add_mcp(text_mcp);
        if (err != rpc::error::OK())
        {
            std::cout << "failed to add text MCP: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }

        err = CO_AWAIT model->prompt("Please add 40 and 2 with the calculator tool.");
        if (err != rpc::error::OK() || !any_piece_contains(*sink, "calculator.add") || !any_piece_contains(*sink, "42"))
        {
            std::cout << "LLM calculator prompt failed: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }

        err = CO_AWAIT model->prompt("Please uppercase the exact text canopy mcp with the text tool.");
        if (err != rpc::error::OK() || !any_piece_contains(*sink, "text.uppercase")
            || !any_piece_contains(*sink, "CANOPY MCP"))
        {
            std::cout << "LLM text prompt failed: " << static_cast<int>(err) << "\n";
            CO_RETURN false;
        }

        std::vector<rpc::interface_descriptor> text_interfaces;
        err = CO_AWAIT text_mcp->list_interfaces(text_interfaces);
        if (err != rpc::error::OK() || text_interfaces.empty())
        {
            std::cout << "text MCP did not expose interface descriptors\n";
            CO_RETURN false;
        }

        CO_RETURN true;
    }
}

int main()
{
    std::cout << "Canopy LLM/MCP schema demo\n";

#ifdef CANOPY_BUILD_COROUTINE
    auto result = coro::sync_wait(run_harness());
#else
    auto result = run_harness();
#endif

    return result ? 0 : 1;
}
