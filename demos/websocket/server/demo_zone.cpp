// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <rpc/rpc.h>

#include "secret_llama/secret_llama.h"
#include "llama_server_engine.h"

#include "demo_zone.h"

namespace secret_llama
{
    inline namespace v1_0
    {
        class llm_engine;
        error_types create_llama_cpp(std::shared_ptr<secret_llama::v1_0::llm_engine>& engine);
    }
}

namespace websocket_demo
{
    namespace v1
    {
        rpc::shared_ptr<i_calculator> create_websocket_demo_instance(
            const std::shared_ptr<secret_llama::v1_0::llm_engine>& engine,
            const std::shared_ptr<secret_llama::v1_0::loaded_model>& loaded_model,
            std::shared_ptr<rpc::service> service_);

        static std::shared_ptr<secret_llama::v1_0::llm_engine> get_llama_cpp()
        {
            // yuck running out of time
            static std::shared_ptr<secret_llama::v1_0::llm_engine> llmengine_;

            if (!llmengine_)
            {
                auto err = secret_llama::v1_0::create_llama_cpp(llmengine_);
                if (err != secret_llama::v1_0::error_types::OK)
                {
                    return nullptr;
                }
            }
            return llmengine_;
        }

        static std::shared_ptr<secret_llama::v1_0::loaded_model> get_loaded_model()
        {
            // yuck running out of time
            static std::shared_ptr<secret_llama::v1_0::loaded_model> loaded_model_;
            if (!loaded_model_)
            {

                void* data = nullptr;
                uint64_t size = 0;

                // https://huggingface.co/unsloth/Qwen3-0.6B-GGUF?show_file_info=Qwen3-0.6B-BF16.gguf
                auto err
                    = parse_model(secret_llama::v1_0::llm_model{.name = "Qwen3-0.6B-BF16",
                                      .local_path = "/var/home/edward/Models/Qwen3-0.6B.gguf/BF16/Qwen3-0.6B-BF16.gguf",
                                      .url = "",
                                      .description = "",
                                      .engine_type = secret_llama::llm_engine_type::LLAMA_CPP,
                                      .engine_config = {},
                                      .encryption_type = secret_llama::encryption_type::NONE,
                                      .encryption_key = "",
                                      .hash_type = secret_llama::hash_type::NONE,
                                      .hash = {},
                                      // file_system::download_status    status;
                                      .is_loaded = true,
                                      .access = secret_llama::access::PUBLIC,
                                      .inactivitiy_timeout = 10000000000},
                        data,
                        size,
                        loaded_model_);
                if (err != secret_llama::v1_0::error_types::OK)
                {
                    return nullptr;
                }
            }
            return loaded_model_;
        }

        websocket_service::websocket_service(std::string name, rpc::zone zone_id, std::shared_ptr<coro::scheduler> scheduler)
            : rpc::root_service(name.data(), zone_id, scheduler)
        {
        }

        websocket_service::websocket_service(
            std::string name, const rpc::service_config& config, std::shared_ptr<coro::scheduler> scheduler)
            : rpc::root_service(name.data(), config, scheduler)
        {
        }

        rpc::shared_ptr<i_calculator> websocket_service::get_demo_instance()
        {
            if (!demo_)
            {
#ifdef _DEBUG
                llama_log_set(
                    [](enum ggml_log_level level, const char* text, void* /* user_data */)
                    {
                        char* text_nonconst = (char*)text;
                        auto len = strlen(text);
                        for (size_t i = 0; i < len; ++i)
                        {
                            if (text_nonconst[i] == '\n')
                            {
                                text_nonconst[i] = ' ';
                            }
                        }

                        switch (level)
                        {
                        case GGML_LOG_LEVEL_CONT: // continue previous log not sure what to put it under as logger is stateless
                        case GGML_LOG_LEVEL_DEBUG:
                            RPC_DEBUG("{}", text);
                            break;
                        case GGML_LOG_LEVEL_INFO:
                            RPC_INFO("{}", text);
                            break;
                        case GGML_LOG_LEVEL_WARN:
                            RPC_WARNING("{}", text);
                            break;
                        case GGML_LOG_LEVEL_ERROR:
                            RPC_ERROR("{}", text);
                            break;
                        case GGML_LOG_LEVEL_NONE:
                        default:
                            break;
                        }
                    },
                    nullptr);
#endif

                demo_ = create_websocket_demo_instance(get_llama_cpp(), get_loaded_model(), shared_from_this());
            }
            return demo_;
        }

    }
}
