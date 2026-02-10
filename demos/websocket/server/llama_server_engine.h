// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// this is code kindly provided by Cedric Wahl as part of source code from secretarium
// its internal project name was secret_llama and was used to demonstrate the use of
// llms inside TEEs
// this is smashed in for a demo

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <memory>
#include <string.h>

// Suppress unused function warnings from llama.cpp headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <chat.h>

#define FILE int

#include <llama.h>
#include <llama-cpp.h>

#undef FILE
#pragma GCC diagnostic pop

// #include <platform/platform.h>
// #include <sdk_v3/sdk.h>

// #include <secret_llama/llm_engine.h>

namespace secret_llama
{
    inline namespace v1_0
    {
        struct context
        {
            virtual int add_prompt(const std::string& prompt) = 0;
            virtual int get_piece(std::string& piece, bool& complete) = 0;
        };

        class loaded_model
        {
        protected:
            llm_model model_config_;

        public:
            loaded_model(const llm_model& model_config)
                : model_config_(model_config)
            {
            }
            virtual ~loaded_model() = default;
            const llm_model& get_config() const { return model_config_; }
        };

        class loaded_tokenizer
        {
        public:
            virtual ~loaded_tokenizer() = default;
        };

        class llm_engine
        {
        public:
            virtual ~llm_engine() = default;

            virtual error_types create_context(const std::shared_ptr<loaded_model>& model_data,
                uint64_t seed,
                const std::string& overrides, // json::v1::map
                std::shared_ptr<context>& context)
                = 0;

            virtual error_types infer(const std::string& prompt,
                const std::string& overrides, // json::v1::map
                uint64_t rng_seed,
                const std::shared_ptr<loaded_tokenizer>& tokenizer_bin,
                const std::shared_ptr<loaded_model>& model_bin,
                std::string& output)
                = 0;

            virtual error_types parse_model(
                const llm_model& modl, void* data, uint64_t size, std::shared_ptr<loaded_model>& loaded_model)
                = 0;
            virtual error_types parse_tokenizer(
                const tokenizer& tok, void* data, uint64_t size, std::shared_ptr<loaded_tokenizer>& loaded_tokeniser)
            {
                std::ignore = tok;
                std::ignore = data;
                std::ignore = size;
                std::ignore = loaded_tokeniser;
                return error_types::NOT_IMPLEMENTED;
            }
        };

        int create_context(const std::shared_ptr<loaded_model>& model,
            uint64_t seed,
            const std::string& overrides, // json::v1::map
            std::shared_ptr<context>& context);

        error_types parse_model(
            const llm_model& modl, void* data, uint64_t size, std::shared_ptr<loaded_model>& loaded_model);
    }
}
