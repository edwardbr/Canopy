/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <llm_mcp/logic.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <random>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>

#include <llama.h>

namespace llm_mcp
{
    namespace v1
    {
        namespace
        {
            constexpr int max_context_tokens = 4096;
            constexpr int max_response_tokens = 192;

            struct llama_model_deleter
            {
                void operator()(llama_model* ptr) const
                {
                    if (ptr)
                        llama_model_free(ptr);
                }
            };

            struct llama_context_deleter
            {
                void operator()(llama_context* ptr) const
                {
                    if (ptr)
                        llama_free(ptr);
                }
            };

            struct llama_sampler_deleter
            {
                void operator()(llama_sampler* ptr) const
                {
                    if (ptr)
                        llama_sampler_free(ptr);
                }
            };

            struct bound_tool
            {
                rpc::shared_ptr<i_mcp> mcp;
                mcp_tool tool;
            };

            class loaded_llama_model
            {
                std::string path_;
                std::unique_ptr<llama_model, llama_model_deleter> model_;

            public:
                loaded_llama_model(
                    std::string path,
                    llama_model* model)
                    : path_(std::move(path))
                    , model_(model)
                {
                }

                [[nodiscard]] llama_model* get() const { return model_.get(); }
                [[nodiscard]] const std::string& path() const { return path_; }
            };

            [[nodiscard]] std::string make_tool_prompt(
                const llm_options& options,
                const std::vector<bound_tool>& tools,
                const std::string& prompt)
            {
                std::ostringstream out;
                out << "System: " << options.system_prompt << "\n\n";
                out << "You are controlling Canopy RPC objects through MCP tools.\n";
                out << "Choose exactly one tool. Reply with JSON only. Do not use markdown.\n";
                out << "The JSON shape is {\"tool\":\"tool.name\",\"arguments\":{...}}.\n";
                out << "The arguments object must match the selected tool input schema exactly.\n\n";
                out << "Available tools:\n";
                for (const auto& tool : tools)
                {
                    out << "name: " << tool.tool.name << "\n";
                    out << "description: " << tool.tool.description << "\n";
                    out << "input_schema: " << tool.tool.input_schema << "\n\n";
                }
                out << "User: " << prompt << "\n";
                out << "Assistant JSON:";
                return out.str();
            }

            [[nodiscard]] bool extract_json_string_field(
                const std::string& text,
                const std::string& field,
                std::string& value)
            {
                const std::regex pattern("\"" + field + "\"\\s*:\\s*\"([^\"]+)\"");
                std::smatch match;
                if (!std::regex_search(text, match, pattern))
                    return false;
                value = match[1].str();
                return true;
            }

            [[nodiscard]] bool extract_json_object_field(
                const std::string& text,
                const std::string& field,
                std::string& value)
            {
                const auto field_pos = text.find("\"" + field + "\"");
                if (field_pos == std::string::npos)
                    return false;

                const auto colon_pos = text.find(':', field_pos);
                if (colon_pos == std::string::npos)
                    return false;

                const auto object_start = text.find('{', colon_pos);
                if (object_start == std::string::npos)
                    return false;

                int depth = 0;
                bool in_string = false;
                bool escaped = false;
                for (size_t index = object_start; index < text.size(); ++index)
                {
                    const char ch = text[index];
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (ch == '\\' && in_string)
                    {
                        escaped = true;
                        continue;
                    }
                    if (ch == '"')
                    {
                        in_string = !in_string;
                        continue;
                    }
                    if (in_string)
                        continue;
                    if (ch == '{')
                        ++depth;
                    else if (ch == '}')
                    {
                        --depth;
                        if (depth == 0)
                        {
                            value = text.substr(object_start, index - object_start + 1);
                            return true;
                        }
                    }
                }
                return false;
            }

            [[nodiscard]] const bound_tool* find_tool(
                const std::vector<bound_tool>& tools,
                const std::string& name)
            {
                for (const auto& tool : tools)
                {
                    if (tool.tool.name == name)
                        return &tool;
                }
                return nullptr;
            }

            [[nodiscard]] std::vector<llama_token> tokenize_prompt(
                const llama_vocab* vocab,
                const std::string& prompt)
            {
                const int token_count
                    = -llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
                if (token_count <= 0)
                    return {};

                std::vector<llama_token> tokens(static_cast<size_t>(token_count));
                const int actual_count = llama_tokenize(
                    vocab,
                    prompt.data(),
                    static_cast<int32_t>(prompt.size()),
                    tokens.data(),
                    static_cast<int32_t>(tokens.size()),
                    true,
                    true);
                if (actual_count < 0)
                    return {};

                tokens.resize(static_cast<size_t>(actual_count));
                return tokens;
            }

            [[nodiscard]] std::string token_to_piece(
                const llama_vocab* vocab,
                llama_token token)
            {
                char buffer[256];
                const int length = llama_token_to_piece(vocab, token, buffer, sizeof(buffer), 0, true);
                if (length <= 0)
                    return {};
                return std::string(buffer, buffer + length);
            }

            [[nodiscard]] uint32_t worker_threads()
            {
                const auto detected = std::thread::hardware_concurrency();
                if (detected == 0)
                    return 4;
                return std::min<uint32_t>(detected, 8);
            }

            class llama_cpp_llm final : public rpc::base<llama_cpp_llm, i_llm>
            {
                llm_options options_;
                std::shared_ptr<loaded_llama_model> model_;
                std::vector<rpc::shared_ptr<i_mcp>> mcps_;
                rpc::shared_ptr<i_llm_events> events_;

                CORO_TASK(llm_mcp_error) emit(const std::string& message)
                {
                    if (!events_)
                        CO_RETURN llm_mcp_error::CALLBACK_NOT_ASSIGNED;
                    CO_RETURN CO_AWAIT events_->piece(message);
                }

                CORO_TASK(llm_mcp_error) finish()
                {
                    if (!events_)
                        CO_RETURN llm_mcp_error::CALLBACK_NOT_ASSIGNED;
                    CO_RETURN CO_AWAIT events_->complete();
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
                        {
                            if (!tool.marshalls_interfaces)
                                out.push_back(bound_tool{mcp, std::move(tool)});
                        }
                    }
                    CO_RETURN rpc::error::OK();
                }

                [[nodiscard]] llm_mcp_error infer(
                    const std::string& prompt,
                    std::string& response) const
                {
                    response.clear();
                    if (!model_ || !model_->get())
                        return llm_mcp_error::LLM_BACKEND_FAILURE;

                    const llama_vocab* vocab = llama_model_get_vocab(model_->get());
                    auto prompt_tokens = tokenize_prompt(vocab, prompt);
                    if (prompt_tokens.empty())
                        return llm_mcp_error::LLM_BACKEND_FAILURE;
                    if (prompt_tokens.size() + max_response_tokens >= static_cast<size_t>(max_context_tokens))
                        return llm_mcp_error::INVALID_ARGUMENT;

                    auto context_params = llama_context_default_params();
                    context_params.n_ctx = max_context_tokens;
                    context_params.n_batch = max_context_tokens;
                    context_params.n_ubatch = std::min<int32_t>(512, max_context_tokens);
                    context_params.n_threads = static_cast<int32_t>(worker_threads());
                    context_params.n_threads_batch = static_cast<int32_t>(worker_threads());

                    std::unique_ptr<llama_context, llama_context_deleter> context(
                        llama_init_from_model(model_->get(), context_params));
                    if (!context)
                        return llm_mcp_error::LLM_BACKEND_FAILURE;

                    std::unique_ptr<llama_sampler, llama_sampler_deleter> sampler(
                        llama_sampler_chain_init(llama_sampler_chain_default_params()));
                    if (!sampler)
                        return llm_mcp_error::LLM_BACKEND_FAILURE;
                    llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());

                    llama_batch batch
                        = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));

                    for (int generated = 0; generated < max_response_tokens; ++generated)
                    {
                        if (llama_decode(context.get(), batch))
                            return llm_mcp_error::LLM_BACKEND_FAILURE;

                        llama_token token = llama_sampler_sample(sampler.get(), context.get(), -1);
                        if (llama_vocab_is_eog(vocab, token))
                            break;

                        response += token_to_piece(vocab, token);
                        std::string tool_name;
                        std::string arguments_json;
                        if (extract_json_string_field(response, "tool", tool_name)
                            && extract_json_object_field(response, "arguments", arguments_json))
                        {
                            break;
                        }

                        batch = llama_batch_get_one(&token, 1);
                    }

                    return rpc::error::OK();
                }

            public:
                llama_cpp_llm(
                    llm_options options,
                    std::shared_ptr<loaded_llama_model> model)
                    : options_(std::move(options))
                    , model_(std::move(model))
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

                    std::ostringstream discovered;
                    discovered << "discovered tools:";
                    for (const auto& tool : tools)
                        discovered << " " << tool.tool.name;
                    err = CO_AWAIT emit(discovered.str());
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    std::string model_response;
                    err = infer(make_tool_prompt(options_, tools, prompt), model_response);
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    err = CO_AWAIT emit("llm response " + model_response);
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    std::string tool_name;
                    std::string arguments_json;
                    if (!extract_json_string_field(model_response, "tool", tool_name)
                        || !extract_json_object_field(model_response, "arguments", arguments_json))
                    {
                        CO_RETURN llm_mcp_error::LLM_BACKEND_FAILURE;
                    }

                    const auto* tool = find_tool(tools, tool_name);
                    if (!tool)
                        CO_RETURN llm_mcp_error::TOOL_NOT_FOUND;

                    err = CO_AWAIT emit("calling " + tool->tool.name + " with generated json " + arguments_json);
                    if (err != rpc::error::OK())
                        CO_RETURN err;

                    std::string result_json;
                    err = CO_AWAIT tool->mcp->call_tool(tool->tool.name, arguments_json, result_json);
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
                    if (err != rpc::error::OK())
                        CO_RETURN err;
                    CO_RETURN CO_AWAIT finish();
                }
            };

            class llama_cpp_llm_factory final : public rpc::base<llama_cpp_llm_factory, i_llm_factory>
            {
                std::string model_path_;
                std::shared_ptr<loaded_llama_model> model_;

                [[nodiscard]] bool ensure_model_loaded()
                {
                    if (model_)
                        return true;

                    llama_log_set([](ggml_log_level, const char*, void*) { }, nullptr);
                    llama_backend_init();
                    (void)ggml_backend_cpu_init();

                    auto model_params = llama_model_default_params();
                    model_params.n_gpu_layers = 0;
                    llama_model* raw_model = llama_model_load_from_file(model_path_.c_str(), model_params);
                    if (!raw_model)
                        return false;

                    model_ = std::make_shared<loaded_llama_model>(model_path_, raw_model);
                    return true;
                }

            public:
                explicit llama_cpp_llm_factory(std::string model_path)
                    : model_path_(std::move(model_path))
                {
                }

                CORO_TASK(llm_mcp_error)
                create_llm(
                    const llm_options& options,
                    rpc::shared_ptr<i_llm>& llm) override
                {
                    if (model_path_.empty() || !ensure_model_loaded())
                        CO_RETURN llm_mcp_error::LLM_BACKEND_FAILURE;

                    llm = rpc::shared_ptr<i_llm>(new llama_cpp_llm(options, model_));
                    CO_RETURN rpc::error::OK();
                }
            };
        }

        rpc::shared_ptr<i_llm_factory> make_llama_cpp_llm_factory(std::string model_path)
        {
            return rpc::shared_ptr<i_llm_factory>(new llama_cpp_llm_factory(std::move(model_path)));
        }

        rpc::shared_ptr<i_llm_factory> make_environment_llm_factory()
        {
            const char* model_path = std::getenv("CANOPY_LLM_MCP_MODEL");
            if (model_path && model_path[0] != '\0')
                return make_llama_cpp_llm_factory(model_path);
            return make_demo_llm_factory();
        }
    }
}
