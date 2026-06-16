/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <llm_mcp/logic.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <utility>

namespace llm_mcp
{
    namespace v1
    {
        namespace
        {
            [[nodiscard]] bool contains_token(
                const std::string& value,
                const std::string& token)
            {
                auto lower_value = value;
                auto lower_token = token;
                std::transform(
                    lower_value.begin(),
                    lower_value.end(),
                    lower_value.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                std::transform(
                    lower_token.begin(),
                    lower_token.end(),
                    lower_token.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return lower_value.find(lower_token) != std::string::npos;
            }

            [[nodiscard]] bool ends_with(
                const std::string& value,
                const std::string& suffix)
            {
                return value.size() >= suffix.size()
                       && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
            }

            [[nodiscard]] std::string escape_json_string(const std::string& value)
            {
                std::string out;
                out.reserve(value.size());
                for (char ch : value)
                {
                    switch (ch)
                    {
                    case '\\':
                        out += "\\\\";
                        break;
                    case '"':
                        out += "\\\"";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        out += ch;
                        break;
                    }
                }
                return out;
            }

            [[nodiscard]] std::vector<int> extract_ints(const std::string& prompt)
            {
                std::vector<int> values;
                const std::regex number_pattern("-?\\d+");
                for (auto it = std::sregex_iterator(prompt.begin(), prompt.end(), number_pattern);
                    it != std::sregex_iterator();
                    ++it)
                {
                    values.push_back(std::stoi(it->str()));
                }
                return values;
            }

            [[nodiscard]] std::string make_tool_name(
                const std::string& target_name,
                const std::string& method_name)
            {
                if (target_name.empty())
                    return method_name;
                return target_name + "." + method_name;
            }

            void append_tools_for_descriptor(
                const std::string& target_name,
                const rpc::interface_descriptor& descriptor,
                std::vector<mcp_tool>& tools)
            {
                for (const auto& method : descriptor.methods)
                {
                    if (method.marshalls_interfaces || method.post)
                        continue;

                    mcp_tool tool;
                    tool.name = make_tool_name(target_name, method.name);
                    tool.description = method.description;
                    tool.interface_name = descriptor.qualified_name;
                    tool.method_name = method.name;
                    tool.interface_id = descriptor.interface_id;
                    tool.method_id = method.id;
                    tool.tag = method.tag;
                    tool.marshalls_interfaces = method.marshalls_interfaces;
                    tool.input_schema = method.in_json_schema;
                    tool.output_schema = method.out_json_schema;
                    tools.push_back(std::move(tool));
                }
            }

            class demo_llm final : public rpc::base<demo_llm, i_llm>
            {
                llm_options options_;
                std::vector<rpc::shared_ptr<i_mcp>> mcps_;
                rpc::shared_ptr<i_llm_events> events_;

                struct bound_tool
                {
                    rpc::shared_ptr<i_mcp> mcp;
                    mcp_tool tool;
                };

                CORO_TASK(llm_mcp_error) emit(const std::string& message)
                {
                    if (!events_)
                        CO_RETURN llm_mcp_error::CALLBACK_NOT_ASSIGNED;
                    auto err = CO_AWAIT events_->piece(message);
                    CO_RETURN err;
                }

                CORO_TASK(llm_mcp_error) finish()
                {
                    if (!events_)
                        CO_RETURN llm_mcp_error::CALLBACK_NOT_ASSIGNED;
                    auto err = CO_AWAIT events_->complete();
                    CO_RETURN err;
                }

                CORO_TASK(llm_mcp_error) collect_tools(std::vector<bound_tool>& out)
                {
                    out.clear();
                    for (const auto& mcp : mcps_)
                    {
                        if (!mcp)
                            continue;

                        std::vector<mcp_tool> tools;
                        auto err = CO_AWAIT mcp->list_tools(tools);
                        if (err != rpc::error::OK())
                            CO_RETURN err;

                        for (auto& tool : tools)
                            out.push_back(bound_tool{mcp, std::move(tool)});
                    }
                    CO_RETURN rpc::error::OK();
                }

                static const bound_tool* find_tool(
                    const std::vector<bound_tool>& tools,
                    const std::string& suffix)
                {
                    for (const auto& tool : tools)
                    {
                        if (ends_with(tool.tool.name, suffix))
                            return &tool;
                    }
                    return nullptr;
                }

                CORO_TASK(llm_mcp_error)
                call_tool(
                    const bound_tool& tool,
                    const std::string& arguments_json)
                {
                    auto err = CO_AWAIT emit("calling " + tool.tool.name + " with generated json " + arguments_json);
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    std::string result_json;
                    err = CO_AWAIT tool.mcp->call_tool(tool.tool.name, arguments_json, result_json);
                    if (err != rpc::error::OK())
                    {
                        auto emit_err = CO_AWAIT emit(
                            "tool call failed with error " + std::to_string(static_cast<int>(err)) + " result "
                            + result_json);
                        if (emit_err != rpc::error::OK())
                            CO_RETURN emit_err;
                        CO_RETURN err;
                    }

                    err = CO_AWAIT emit("tool result " + result_json);
                    CO_RETURN err;
                }

            public:
                explicit demo_llm(llm_options options)
                    : options_(std::move(options))
                {
                }

                CORO_TASK(llm_mcp_error) add_mcp(const rpc::shared_ptr<i_mcp>& mcp) override
                {
                    if (!mcp)
                        CO_RETURN llm_mcp_error::INVALID_ARGUMENT;
                    mcps_.push_back(mcp);
                    CO_RETURN rpc::error::OK();
                }

                CORO_TASK(llm_mcp_error) set_callback(const rpc::shared_ptr<i_llm_events>& events) override
                {
                    if (!events)
                        CO_RETURN llm_mcp_error::INVALID_ARGUMENT;
                    events_ = events;
                    CO_RETURN rpc::error::OK();
                }

                CORO_TASK(llm_mcp_error) prompt(const std::string& prompt) override
                {
                    if (!events_)
                        CO_RETURN llm_mcp_error::CALLBACK_NOT_ASSIGNED;

                    std::vector<bound_tool> tools;
                    auto err = CO_AWAIT collect_tools(tools);
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    err = CO_AWAIT emit("system prompt: " + options_.system_prompt);
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    std::ostringstream discovered;
                    discovered << "discovered tools:";
                    for (const auto& tool : tools)
                        discovered << " " << tool.tool.name;
                    err = CO_AWAIT emit(discovered.str());
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    if (contains_token(prompt, "add") || contains_token(prompt, "sum"))
                    {
                        const auto* tool = find_tool(tools, ".add");
                        if (!tool)
                            CO_RETURN llm_mcp_error::TOOL_NOT_FOUND;

                        auto values = extract_ints(prompt);
                        const int a = values.size() > 0 ? values[0] : 40;
                        const int b = values.size() > 1 ? values[1] : 2;
                        err = CO_AWAIT call_tool(
                            *tool, "{\"a\":" + std::to_string(a) + ",\"b\":" + std::to_string(b) + "}");
                        if (err != rpc::error::OK())
                            CO_RETURN err;
                        CO_RETURN CO_AWAIT finish();
                    }

                    if (contains_token(prompt, "uppercase"))
                    {
                        const auto* tool = find_tool(tools, ".uppercase");
                        if (!tool)
                            CO_RETURN llm_mcp_error::TOOL_NOT_FOUND;

                        err = CO_AWAIT call_tool(*tool, "{\"input\":\"canopy mcp\"}");
                        if (err != rpc::error::OK())
                            CO_RETURN err;
                        CO_RETURN CO_AWAIT finish();
                    }

                    if (contains_token(prompt, "concat") || contains_token(prompt, "join"))
                    {
                        const auto* tool = find_tool(tools, ".concat");
                        if (!tool)
                            CO_RETURN llm_mcp_error::TOOL_NOT_FOUND;

                        err = CO_AWAIT call_tool(*tool, "{\"left\":\"schema\",\"right\":\"tool\"}");
                        if (err != rpc::error::OK())
                            CO_RETURN err;
                        CO_RETURN CO_AWAIT finish();
                    }

                    err = CO_AWAIT emit("no tool selected for prompt: " + escape_json_string(prompt));
                    if (err != rpc::error::OK())
                        CO_RETURN err;
                    CO_RETURN CO_AWAIT finish();
                }
            };

            class demo_llm_factory final : public rpc::base<demo_llm_factory, i_llm_factory>
            {
            public:
                CORO_TASK(llm_mcp_error)
                create_llm(
                    const llm_options& options,
                    rpc::shared_ptr<i_llm>& llm) override
                {
                    llm = rpc::shared_ptr<i_llm>(new demo_llm(options));
                    CO_RETURN rpc::error::OK();
                }
            };
        }

        schema_mcp::schema_mcp(
            std::string target_name,
            rpc::shared_ptr<rpc::casting_interface> target)
            : target_name_(std::move(target_name))
            , target_(std::move(target))
        {
        }

        CORO_TASK(llm_mcp_error) schema_mcp::list_interfaces(std::vector<rpc::interface_descriptor>& interfaces)
        {
            interfaces.clear();
            if (!target_)
                CO_RETURN llm_mcp_error::INVALID_ARGUMENT;

            CO_RETURN CO_AWAIT rpc::casting_interface::get_schema(
                *target_, interfaces, rpc::encoding::yas_json, rpc::schema_flavor::mcp, false);
        }

        CORO_TASK(llm_mcp_error) schema_mcp::list_tools(std::vector<mcp_tool>& tools)
        {
            tools.clear();
            std::vector<rpc::interface_descriptor> descriptors;
            auto err = CO_AWAIT list_interfaces(descriptors);
            if (err != rpc::error::OK())
                CO_RETURN err;

            for (const auto& descriptor : descriptors)
            {
                if (descriptor.deprecated)
                    continue;
                append_tools_for_descriptor(target_name_, descriptor, tools);
            }
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(llm_mcp_error)
        schema_mcp::call_tool(
            const std::string& tool_name,
            const std::string& arguments_json,
            std::string& result_json)
        {
            result_json.clear();
            if (!target_)
                CO_RETURN llm_mcp_error::INVALID_ARGUMENT;

            std::vector<rpc::interface_descriptor> descriptors;
            auto err = CO_AWAIT list_interfaces(descriptors);
            if (err != rpc::error::OK())
                CO_RETURN err;

            for (const auto& descriptor : descriptors)
            {
                if (descriptor.deprecated)
                    continue;

                for (const auto& method : descriptor.methods)
                {
                    if (make_tool_name(target_name_, method.name) != tool_name)
                        continue;

                    if (method.marshalls_interfaces || method.post)
                        CO_RETURN llm_mcp_error::TOOL_UNSUPPORTED;

                    rpc::send_params params{};
                    params.protocol_version = rpc::get_version();
                    params.encoding_type = rpc::encoding::yas_json;
                    params.tag = method.tag;
                    params.interface_id = descriptor.interface_id;
                    params.method_id = method.id;
                    params.in_data.assign(arguments_json.begin(), arguments_json.end());

                    auto result = CO_AWAIT rpc::casting_interface::call(*target_, std::move(params));

                    result_json.assign(result.out_buf.begin(), result.out_buf.end());
                    if (result.error_code == rpc::error::OK())
                        CO_RETURN rpc::error::OK();

                    if (result_json.empty())
                        result_json = "{\"error_code\":" + std::to_string(result.error_code) + "}";
                    if (result.error_code < 0)
                        CO_RETURN result.error_code;
                    CO_RETURN llm_mcp_error::TOOL_CALL_FAILED;
                }
            }

            CO_RETURN llm_mcp_error::TOOL_NOT_FOUND;
        }

        rpc::shared_ptr<i_mcp> make_schema_mcp(
            std::string target_name,
            rpc::shared_ptr<rpc::casting_interface> target)
        {
            return rpc::shared_ptr<i_mcp>(new schema_mcp(std::move(target_name), std::move(target)));
        }

        rpc::shared_ptr<i_llm_factory> make_demo_llm_factory()
        {
            return rpc::shared_ptr<i_llm_factory>(new demo_llm_factory());
        }

#ifndef CANOPY_LLM_MCP_HAS_LLAMA_CPP
        rpc::shared_ptr<i_llm_factory> make_llama_cpp_llm_factory(std::string model_path)
        {
            std::ignore = model_path;
            return nullptr;
        }

        rpc::shared_ptr<i_llm_factory> make_environment_llm_factory()
        {
            const char* model_path = std::getenv("CANOPY_LLM_MCP_MODEL");
            if (model_path && model_path[0] != '\0')
                return make_llama_cpp_llm_factory(model_path);
            return make_demo_llm_factory();
        }
#endif
    }
}
