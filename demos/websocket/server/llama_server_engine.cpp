// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// this is code kindly provided by Cedric Wahl as part of source code from secretarium
// its internal project name was secret_llama and was used to demonstrate the use of
// llms inside TEEs
// this is smashed in for a demo

/* Inference for Llama-2 Transformer model in pure C */

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
#include <common.h>
#include <chat.h>

#define FILE int

#include <llama.h>
#include <llama-cpp.h>

#undef FILE
#pragma GCC diagnostic pop

// #include <platform/platform.h>
// #include <sdk_v3/sdk.h>

// #include <secret_llama/llm_engine.h>

#include <secret_llama/secret_llama.h>
#include "llama_server_engine.h"

#include <json/json.h>

#define POOL_THREADS 4

namespace secret_llama
{
    inline namespace v1_0
    {
        class llama_cpp_loaded_model : public loaded_model
        {
            llama_model* model_ = nullptr;

        public:
            llama_cpp_loaded_model(llama_model* model, const llm_model& model_config)
                : loaded_model(model_config)
                , model_(model)
            {
            }
            virtual ~llama_cpp_loaded_model()
            {
                if (model_)
                    llama_model_free(model_);
            }

            llama_model* get() const { return model_; }
        };

        struct context_deleter
        {
            void operator()(llama_context* ptr) const
            {
                if (ptr)
                    llama_free(ptr);
            }
        };

        struct sampler_deleter
        {
            void operator()(llama_sampler* ptr) const
            {
                if (ptr)
                    llama_sampler_free(ptr);
            }
        };

        struct llama_cpp_context : public context
        {
            uint64_t seed_ = 0;
            json::v1::map config_;

            std::shared_ptr<llama_cpp_loaded_model> model_;
            std::unique_ptr<llama_context, context_deleter> ctx_;
            std::unique_ptr<llama_sampler, sampler_deleter> sampler_;

            struct chat_message
            {
                const char* role;
                std::string content;
            };
            std::list<chat_message> msg_strs_;
            bool use_jinja_ = false;
            int prev_len_ = 0;

            std::vector<char> formatted_;

            llama_batch batch_;
            llama_token new_token_id_;
            common_chat_templates_ptr chat_templates_;
            // std::vector<std::string> stop_strs_; // To hold stop strings from configuration

            std::string current_response_;

            std::vector<llama_token> batch_tokens_;

            bool user_turn_ = true;
            bool first_pass_ = true;
            uint64_t context_id_;

            // Add this to the llama_cpp_context class private members:
            std::vector<std::string> stop_strs_;
            std::string accumulated_response_; // Buffer to check for stop strings

            // In the constructor, initialize the stop strings:
            llama_cpp_context(
                uint64_t seed, std::shared_ptr<llama_cpp_loaded_model> model, const std::string& overrides, uint64_t context_id)
                : seed_(seed)
                , model_(model)
                , context_id_(context_id)
            {
                RPC_DEBUG("[CTX {}] CONSTRUCTOR: Creating new context.", std::to_string(context_id_));

                // Initialize stop strings based on chat template
                stop_strs_ = {"<|im_end|>", "</s>", "<|endoftext|>"};

                // deep copy
                if (!overrides.empty())
                {
                    std::string error = rpc::from_yas_json(overrides, config_);
                    if (!error.empty())
                    {
                        RPC_ASSERT(false);
                    }
                }
            }

            virtual ~llama_cpp_context()
            {
                RPC_DEBUG("[CTX {}] DESTRUCTOR: Freeing context.", std::to_string(context_id_));
            }

            error_types init()
            {
                RPC_DEBUG("[CTX {}] Initializing context...", std::to_string(context_id_));
                try
                {
                    // initialize the context
                    llama_context_params ctx_params = llama_context_default_params();
                    ctx_params.n_ctx = ctx_params.n_batch;

                    // just the basics for now
                    {
                        {
                            auto it = config_.find("n_ctx");
                            if (it != config_.end())
                            {
                                ctx_params.n_ctx = it->second.convert_to_int<uint32_t>();
                            }
                            RPC_DEBUG("setting n_ctx = {}", ctx_params.n_ctx);
                        }
                        {
                            auto it = config_.find("n_batch");
                            if (it != config_.end())
                            {
                                ctx_params.n_batch = it->second.convert_to_int<uint32_t>();
                            }
                            RPC_DEBUG("setting n_batch = {}", ctx_params.n_batch);
                        }

                        // these thread parameters should typically not be used in production
                        {
                            auto it = config_.find("n_threads");
                            if (it != config_.end())
                            {
                                ctx_params.n_threads = it->second.convert_to_int<int32_t>();
                            }
                            else
                            {
                                ctx_params.n_threads = POOL_THREADS;
                            }
                            RPC_DEBUG("setting n_threads = {}", ctx_params.n_threads);
                        }
                        {
                            auto it = config_.find("n_threads_batch");
                            if (it != config_.end())
                            {
                                ctx_params.n_threads_batch = it->second.convert_to_int<int32_t>();
                            }
                            else
                            {
                                ctx_params.n_threads_batch = POOL_THREADS;
                            }
                            RPC_DEBUG("setting n_threads_batch = {}", ctx_params.n_threads_batch);
                        }
                    }

                    ctx_ = std::unique_ptr<llama_context, context_deleter>(
                        llama_init_from_model(model_->get(), ctx_params));
                    if (!ctx_)
                    {
                        RPC_ERROR("[CTX {}] failed to create the llama_context", std::to_string(context_id_));
                        return error_types::INVALID_CONTEXT;
                    }

                    RPC_DEBUG(
                        "calculated_kv_cache_size {}", std::to_string(calculate_kv_cache_size(ctx_.get(), ctx_params)));

                    // initialize the sampler chain
                    sampler_ = std::unique_ptr<llama_sampler, sampler_deleter>(
                        llama_sampler_chain_init(llama_sampler_chain_default_params()));

                    llama_sampler_chain_add(sampler_.get(), llama_sampler_init_dist(seed_));

                    {
                        auto it = config_.find("min_p");
                        if (it != config_.end())
                        {
                            auto min_p = it->second.get<float>();
                            llama_sampler_chain_add(sampler_.get(), llama_sampler_init_min_p(min_p, 1));
                            RPC_DEBUG("setting min_p = {}", min_p);
                        }
                    }

                    {
                        auto it = config_.find("topp");
                        if (it != config_.end())
                        {
                            auto top_p = it->second.get<float>();
                            llama_sampler_chain_add(sampler_.get(), llama_sampler_init_top_p(top_p, 1));
                            RPC_DEBUG("setting top_p = {}", top_p);
                        }
                    }

                    {
                        auto it = config_.find("temperature");
                        if (it != config_.end())
                        {
                            auto temperature = it->second.get<float>();
                            llama_sampler_chain_add(sampler_.get(), llama_sampler_init_temp(temperature));
                            RPC_DEBUG("setting temperature = {}", temperature);
                        }
                    }

                    // (sampler setup code omitted for brevity but would be here)

                    std::string chat_template_str;
                    {
                        auto it = config_.find("template");
                        if (it != config_.end())
                        {
                            chat_template_str = it->second.get<std::string>();
                            RPC_DEBUG("template = {}", chat_template_str);
                        }
                    }

                    chat_templates_ = common_chat_templates_init(model_->get(), chat_template_str);
                    RPC_DEBUG("[CTX {}] Chat template: is explicit {}, value: \"{}\"",
                        std::to_string(context_id_),
                        common_chat_templates_was_explicit(chat_templates_.get()) ? "true" : "false",
                        common_chat_templates_source(chat_templates_.get()));

                    // *** NEW: Extract stop strings from the model metadata ***
                    const llama_model* model = model_->get();

                    // Get EOS token from model metadata - this should be the primary stop token
                    const llama_vocab* vocab = llama_model_get_vocab(model);
                    llama_token eos_token_id = llama_vocab_type(vocab) == LLAMA_VOCAB_TYPE_SPM ? llama_vocab_eos(vocab)
                                                                                               : llama_vocab_eot(vocab);

                    // Convert EOS token ID to string
                    if (eos_token_id != LLAMA_TOKEN_NULL)
                    {
                        char eos_buf[256];
                        int eos_len = llama_token_to_piece(vocab, eos_token_id, eos_buf, sizeof(eos_buf), 0, true);
                        if (eos_len > 0)
                        {
                            std::string eos_str(eos_buf, eos_len);
                            stop_strs_.push_back(eos_str);
                            RPC_DEBUG("[CTX {}] Added EOS stop string: '{}'", std::to_string(context_id_), eos_str);
                        }
                    }

                    // Try to get stop strings from model metadata
                    // Check for common chat template stop tokens in metadata
                    char metadata_buf[1024];

                    // Try to get the EOS token from tokenizer metadata
                    if (llama_model_meta_val_str(model, "tokenizer.ggml.eos_token", metadata_buf, sizeof(metadata_buf)) >= 0)
                    {
                        std::string meta_eos = metadata_buf;
                        if (!meta_eos.empty()
                            && std::find(stop_strs_.begin(), stop_strs_.end(), meta_eos) == stop_strs_.end())
                        {
                            stop_strs_.push_back(meta_eos);
                            RPC_DEBUG(
                                "[CTX {}] Added metadata EOS stop string: '{}'", std::to_string(context_id_), meta_eos);
                        }
                    }

                    // For ChatML template specifically, add the known stop token
                    std::string template_source = common_chat_templates_source(chat_templates_.get());
                    if (template_source.find("<|im_start|>") != std::string::npos
                        && template_source.find("<|im_end|>") != std::string::npos)
                    {
                        // This is a ChatML template
                        if (std::find(stop_strs_.begin(), stop_strs_.end(), "<|im_end|>") == stop_strs_.end())
                        {
                            stop_strs_.push_back("<|im_end|>");
                            RPC_DEBUG("[CTX {}] Added ChatML stop string: '<|im_end|>'", std::to_string(context_id_));
                        }
                    }

                    // Add other common stop strings as fallbacks
                    std::vector<std::string> fallback_stops = {"</s>", "<|endoftext|>", "<|eot_id|>"};
                    for (const auto& stop : fallback_stops)
                    {
                        if (std::find(stop_strs_.begin(), stop_strs_.end(), stop) == stop_strs_.end())
                        {
                            stop_strs_.push_back(stop);
                        }
                    }

                    RPC_DEBUG("[CTX {}] Total stop strings configured: {}", std::to_string(context_id_), stop_strs_.size());
                    for (size_t i = 0; i < stop_strs_.size(); ++i)
                    {
                        RPC_DEBUG("[CTX {}] Stop string [{}]: '{}'", std::to_string(context_id_), i, stop_strs_[i]);
                    }

                    formatted_.resize(llama_n_ctx(ctx_.get()));
                    RPC_DEBUG("[CTX {}] Initialization complete. Context size: {}",
                        std::to_string(context_id_),
                        llama_n_ctx(ctx_.get()));
                    return error_types::OK;
                }
                catch (const std::exception& e)
                {
                    RPC_ERROR("[CTX {}] llamacpp exception thrown: {}", std::to_string(context_id_), e.what());
                    return error_types::EXCEPTION_THROWN;
                }
            }

            /**
             * @brief Calculates the total memory size of the KV cache for a given llama_context.
             *
             * This function determines the size in bytes of the Key-Value cache (K-cache and V-cache)
             * by inspecting the context's configuration. It uses the recommended public `llama.h` API
             * to ensure future compatibility.
             *
             * @param ctx A constant pointer to the llama_context to be inspected.
             * @return The total size of the KV cache in bytes as a size_t. Returns 0 if the context is invalid.
             */
            size_t calculate_kv_cache_size(const llama_context* ctx, const llama_context_params& ctx_params)
            {
                if (!ctx)
                {
                    return 0;
                }

                // 1. Get the model instance from the context to query its parameters.
                const llama_model* model = llama_get_model(ctx);
                if (!model)
                {
                    return 0;
                }

                // 2. Get the fundamental dimensions that determine the cache's shape.
                // Note: We use the modern `llama_model_` prefixed functions as they are forward-compatible.
                const int32_t n_ctx = llama_n_ctx(ctx);             // Max context size
                const int32_t n_layer = llama_model_n_layer(model); // Number of layers
                const int32_t n_embd = llama_model_n_embd(model);   // Embedding dimension
                const int32_t n_head = llama_model_n_head(model);   // Number of attention heads

                if (n_head == 0)
                {
                    // Avoid division by zero in case of invalid model parameters
                    return 0;
                }

                const int32_t n_head_kv = llama_model_n_head_kv(model);
                const int64_t n_embd_kv = (int64_t(n_embd) / n_head) * n_head_kv;

                // 3. Calculate the total number of elements in one cache tensor (K or V).
                const int64_t n_elements_per_tensor = (int64_t)n_ctx * n_layer * n_embd_kv;

                // 5. Calculate the size in bytes for each cache tensor using its element count and data type size.
                const size_t size_k_bytes = n_elements_per_tensor * ggml_type_size(ctx_params.type_k);
                const size_t size_v_bytes = n_elements_per_tensor * ggml_type_size(ctx_params.type_v);

                // 6. The total size is the sum of the K-cache and V-cache.
                return size_k_bytes + size_v_bytes;
            }

            // Add a message to `messages` and store its content in `msg_strs`
            void add_message(const char* role, const std::string& text)
            {
                msg_strs_.push_back({role, std::move(text)});
            }

            // Helper function to apply the chat template and handle errors
            int apply_chat_template_with_error_handling(
                const common_chat_templates* tmpls, const bool append, int& output_length)
            {
                const int new_len = apply_chat_template(tmpls, append);
                if (new_len < 0)
                {
                    RPC_ERROR("failed to apply the chat template");
                    return -1;
                }
                output_length = new_len;
                return 0;
            }

            // Function to apply the chat template and resize `formatted` if needed
            int apply_chat_template(const struct common_chat_templates* tmpls, const bool append)
            {
                common_chat_templates_inputs inputs;
                for (const auto& msg : msg_strs_)
                {
                    common_chat_msg cmsg;
                    cmsg.role = msg.role;
                    cmsg.content = msg.content;
                    inputs.messages.push_back(cmsg);
                }
                inputs.add_generation_prompt = append;
                inputs.use_jinja = use_jinja_;

                auto chat_params = common_chat_templates_apply(tmpls, inputs);
                auto result = chat_params.prompt;
                formatted_.resize(result.size() + 1);
                memcpy(formatted_.data(), result.c_str(), result.size() + 1);
                return result.size();
            }

            // Function to tokenize the prompt
            int tokenize_prompt(const std::string& prompt)
            {
                const llama_vocab* vocab = llama_model_get_vocab(model_->get());
                const bool is_first = llama_memory_seq_pos_max(llama_get_memory(ctx_.get()), 0) == -1;

                const int n_prompt_tokens = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, is_first, true);
                batch_tokens_.resize(n_prompt_tokens);
                if (llama_tokenize(
                        vocab, prompt.c_str(), prompt.size(), batch_tokens_.data(), batch_tokens_.size(), is_first, true)
                    < 0)
                {
                    RPC_ERROR("failed to tokenize the prompt");
                    return -1;
                }
                return n_prompt_tokens;
            }

            // Check if we have enough space in the context to evaluate this batch
            int check_context_size()
            {
                const int n_ctx = llama_n_ctx(ctx_.get());
                const int n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(ctx_.get()), 0) + 1;
                if (n_ctx_used + batch_.n_tokens > n_ctx)
                {
                    RPC_ERROR("[CTX {}] LOG: context size exceeded. Used: {}, Batch: {}, Total Ctx: {}",
                        std::to_string(context_id_),
                        n_ctx_used,
                        batch_.n_tokens,
                        n_ctx);
                    return 1;
                }
                return 0;
            }

            // convert the token to a string
            int convert_token_to_string(const llama_token token_id, std::string& piece)
            {
                const llama_vocab* vocab = llama_model_get_vocab(model_->get());
                char buf[256];
                int n = llama_token_to_piece(vocab, token_id, buf, sizeof(buf), 0, true);
                if (n < 0)
                {
                    RPC_ERROR(
                        "[CTX {}] LOG ERROR: failed to convert token {} to piece", std::to_string(context_id_), token_id);
                    return 1;
                }

                piece.assign(buf, buf + n);
                return 0;
            }

            int add_prompt(const std::string& prompt) override
            {
                accumulated_response_.clear(); // Reset for new conversation turn

                RPC_DEBUG("\n[CTX {}] START", std::to_string(context_id_));
                try
                {
                    RPC_DEBUG("[CTX {}] user_turn_={}, first_pass_={}",
                        std::to_string(context_id_),
                        user_turn_ ? "true" : "false",
                        first_pass_ ? "true" : "false");

                    if (!user_turn_)
                    {
                        RPC_DEBUG("[CTX {}] Not user's turn.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::LLM_STILL_PROCESSING);
                    }
                    if (prompt.empty())
                    {
                        RPC_DEBUG("[CTX {}] Empty prompt received.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::INVALID_PROMPT);
                    }

                    if (!first_pass_)
                    {
                        RPC_DEBUG("[CTX {}] Not first pass. Adding previous assistant response to history: \"{}\"",
                            std::to_string(context_id_),
                            current_response_.c_str());
                        add_message("assistant", current_response_);
                        current_response_.clear();
                        if (apply_chat_template_with_error_handling(chat_templates_.get(), false, prev_len_) < 0)
                        {
                            RPC_ERROR("[CTX {}] Unable to apply chat template for assistant message.",
                                std::to_string(context_id_));
                            return to_standard_return_type(error_types::UNABLE_TO_APPLY_CHAT_TEMPLATE);
                        }
                    }

                    if (first_pass_)
                    {
                        auto it = config_.find("system_prompt");
                        if (it != config_.end())
                        {
                            auto system_prompt = it->second.get<std::string>();
                            if (!system_prompt.empty())
                            {
                                RPC_DEBUG("[CTX {}] First pass. Adding system prompt: \"{}\"",
                                    std::to_string(context_id_),
                                    system_prompt.c_str());
                                add_message("system", system_prompt);
                            }
                        }
                    }

                    std::string prompt_str(prompt.begin(), prompt.end());
                    RPC_DEBUG("[CTX {}] Adding user message: \"{}\"", std::to_string(context_id_), prompt_str.c_str());
                    add_message("user", prompt_str);

                    int new_len;
                    if (apply_chat_template_with_error_handling(chat_templates_.get(), true, new_len) < 0)
                    {
                        RPC_ERROR("[CTX {}] Unable to apply chat template for user message.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::UNABLE_TO_APPLY_CHAT_TEMPLATE);
                    }
                    RPC_DEBUG("[CTX {}] Chat template applied. prev_len_={}, new_len={}",
                        std::to_string(context_id_),
                        prev_len_,
                        new_len);

                    std::string prompt_to_tokenize(formatted_.begin() + prev_len_, formatted_.begin() + new_len);
                    RPC_DEBUG("[CTX {}] Final prompt segment to tokenize: \"{}\"",
                        std::to_string(context_id_),
                        prompt_to_tokenize.c_str());

                    int n_tokens = tokenize_prompt(prompt_to_tokenize);
                    if (n_tokens < 0)
                    {
                        RPC_ERROR("[CTX {}] Failed to tokenize prompt.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::UNABLE_TO_APPLY_CHAT_TEMPLATE);
                    }
                    RPC_DEBUG("[CTX {}] Tokenized into {} tokens.", std::to_string(context_id_), n_tokens);

                    // prepare a batch for the prompt
                    batch_ = llama_batch_get_one(batch_tokens_.data(), batch_tokens_.size());
                    RPC_DEBUG("[CTX {}] Batch created. n_tokens={}", 1, batch_.n_tokens);

                    // Update prev_len_ with the latest total length so the *next* call to add_prompt
                    // has the correct starting point for slicing.
                    prev_len_ = new_len;

                    first_pass_ = false;
                    user_turn_ = false;
                    RPC_DEBUG("[CTX {}] END", std::to_string(context_id_));
                    return to_standard_return_type(error_types::OK);
                }
                catch (const std::exception& e)
                {
                    RPC_ERROR("[CTX {}] llamacpp exception thrown: {}", std::to_string(context_id_), e.what());
                    return to_standard_return_type(error_types::EXCEPTION_THROWN);
                }
            }

            // Add this helper method to check for stop strings:
            bool check_stop_strings(const std::string& text, std::string& matched_stop_str)
            {
                for (const auto& stop_str : stop_strs_)
                {
                    if (text.find(stop_str) != std::string::npos)
                    {
                        matched_stop_str = stop_str;
                        return true;
                    }
                }
                return false;
            }

            // Replace the stop string checking section in get_piece() with this:
            int get_piece(std::string& piece, bool& complete) override
            {
                RPC_DEBUG("\n[CTX {}] START", std::to_string(context_id_));
                try
                {
                    complete = false;
                    RPC_DEBUG("[CTX {}] user_turn_={}", std::to_string(context_id_), user_turn_ ? "true" : "false");

                    // If it's the user's turn, generation is complete for this round.
                    if (user_turn_)
                    {
                        complete = true;
                        RPC_DEBUG("[CTX {}] Is user's turn, completing.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::OK);
                    }

                    // Ensure the context has enough space for the tokens in the current batch.
                    if (check_context_size())
                    {
                        complete = true;
                        RPC_DEBUG("[CTX {}] Context size exceeded.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::CONTEXT_SIZE_EXCEEDED);
                    }

                    RPC_DEBUG("Decoding batch... n_tokens={}, KV cache used={}",
                        batch_.n_tokens,
                        (int)(llama_memory_seq_pos_max(llama_get_memory(ctx_.get()), 0) + 1));

                    if (llama_decode(ctx_.get(), batch_))
                    {
                        RPC_ERROR("[CTX {}] llama_decode failed.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::DECODE_FAILURE);
                    }
                    RPC_DEBUG("[CTX {}] Decode successful. KV cache now used={}",
                        std::to_string(context_id_),
                        (int)(llama_memory_seq_pos_max(llama_get_memory(ctx_.get()), 0) + 1));

                    // Sample the next token from the logits produced by llama_decode.
                    new_token_id_ = llama_sampler_sample(sampler_.get(), ctx_.get(), -1);
                    RPC_DEBUG("[CTX {}] Sampled token ID: {}", std::to_string(context_id_), new_token_id_);

                    const llama_vocab* vocab = llama_model_get_vocab(model_->get());

                    // Check for End of Generation token.
                    if (llama_vocab_is_eog(vocab, new_token_id_))
                    {
                        RPC_DEBUG("[CTX {}] End of generation token found.", std::to_string(context_id_));
                        complete = true;
                        user_turn_ = true;
                        accumulated_response_.clear();
                        return to_standard_return_type(error_types::OK);
                    }

                    // Convert token to string
                    if (convert_token_to_string(new_token_id_, piece))
                    {
                        RPC_ERROR("[CTX {}] Failed to convert token to string.", std::to_string(context_id_));
                        return to_standard_return_type(error_types::UNABLE_TO_GET_PIECE);
                    }

                    std::string piece_str(piece.begin(), piece.end());
                    RPC_DEBUG("[CTX {}] Generated piece: \"{}\"", std::to_string(context_id_), piece_str.c_str());

                    // Add to accumulated response for multi-token stop string detection
                    accumulated_response_ += piece_str;
                    current_response_ += piece_str;

                    // Check for stop strings in accumulated response
                    std::string matched_stop_str;
                    if (check_stop_strings(accumulated_response_, matched_stop_str))
                    {
                        RPC_DEBUG("[CTX {}] Stop string '{}' found in accumulated response.",
                            std::to_string(context_id_),
                            matched_stop_str.c_str());

                        // Remove the stop string from current_response_
                        size_t stop_pos = current_response_.find(matched_stop_str);
                        if (stop_pos != std::string::npos)
                        {
                            current_response_.erase(stop_pos);
                        }

                        // Calculate how much of the current piece to return (if any)
                        size_t acc_stop_pos = accumulated_response_.find(matched_stop_str);
                        if (acc_stop_pos != std::string::npos)
                        {
                            // Find how much of the current piece comes before the stop string
                            size_t piece_start_in_acc = accumulated_response_.length() - piece_str.length();
                            if (acc_stop_pos >= piece_start_in_acc)
                            {
                                // Stop string starts within current piece
                                size_t valid_piece_len = acc_stop_pos - piece_start_in_acc;
                                piece.resize(valid_piece_len);
                            }
                            // else: stop string is entirely before current piece, return full piece
                        }

                        complete = true;
                        user_turn_ = true;
                        accumulated_response_.clear(); // Reset for next turn
                        return to_standard_return_type(error_types::OK);
                    }

                    // Trim accumulated response to prevent excessive memory usage
                    const size_t max_buffer_size = 100; // Should be larger than longest stop string
                    if (accumulated_response_.length() > max_buffer_size)
                    {
                        accumulated_response_
                            = accumulated_response_.substr(accumulated_response_.length() - max_buffer_size);
                    }

                    // Prepare next batch
                    batch_ = llama_batch_get_one(&new_token_id_, 1);

                    RPC_DEBUG("[CTX {}] GET_PIECE END", std::to_string(context_id_));
                    return to_standard_return_type(error_types::OK);
                }
                catch (const std::exception& e)
                {
                    RPC_ERROR("[CTX {}] llamacpp exception thrown: {}", std::to_string(context_id_), e.what());
                    return to_standard_return_type(error_types::EXCEPTION_THROWN);
                }
            }
        };

        class llama_cpp_engine : public llm_engine
        {
            std::atomic<uint64_t> count_;

        public:
            virtual ~llama_cpp_engine() = default;

            error_types create_context(const std::shared_ptr<loaded_model>& model,
                uint64_t seed,
                const std::string& overrides, // std::string
                std::shared_ptr<context>& context) override
            {
                auto llama_model = std::static_pointer_cast<llama_cpp_loaded_model>(model);
                auto ctx = std::make_shared<llama_cpp_context>(seed, llama_model, overrides, ++count_);

                auto ret = ctx->init();
                if (ret != error_types::OK)
                    return ret;
                context = ctx;
                return error_types::OK;
            }

            error_types parse_model(
                const llm_model& modl, void* data, uint64_t size, std::shared_ptr<loaded_model>& loaded_model) override
            {
                try
                {
                    std::ignore = modl;

                    llama_model_params model_params = llama_model_default_params();
                    // The goal is to prevent the large, static model weights from displacing the smaller, more frequently
                    // accessed KV Cache from the L3 cache. You will treat the EPC as a "streaming source" for weights.
                    model_params.n_gpu_layers = 0;

                    llama_model* model = llama_model_load_from_file(modl.local_path.c_str(), model_params);
                    if (model == NULL)
                    {
                        return error_types::UNABLE_TO_LOAD_RECORD;
                    }

                    loaded_model = std::make_shared<llama_cpp_loaded_model>(model, modl);
                    return error_types::OK;
                }
                catch (const std::exception& e)
                {
                    RPC_ERROR("llamacpp exception thrown");
                    return error_types::EXCEPTION_THROWN;
                }
            }

            error_types infer(const std::string& prompt,
                const std::string& overrides,
                uint64_t rng_seed,
                const std::shared_ptr<loaded_tokenizer>&,
                const std::shared_ptr<loaded_model>& model,
                std::string& output) override
            {
                try
                {
                    error_types ret = error_types::OK;

                    // number of tokens to predict
                    int n_predict = 32;

                    auto llama_model = std::static_pointer_cast<llama_cpp_loaded_model>(model);

                    const llama_vocab* vocab = llama_model_get_vocab(llama_model->get());

                    const int n_prompt
                        = -llama_tokenize(vocab, (const char*)prompt.data(), prompt.size(), NULL, 0, true, true);

                    std::vector<llama_token> prompt_tokens(n_prompt);
                    if (llama_tokenize(
                            vocab, (const char*)prompt.data(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), true, true)
                        < 0)
                    {
                        return error_types::UNABLE_TO_TOKENIZE;
                    }

                    llama_context_params ctx_params = llama_context_default_params();

                    ctx_params.n_ctx = ctx_params.n_batch;

                    // n_ctx is the context size
                    ctx_params.n_ctx = n_prompt + n_predict - 1;
                    // n_batch is the maximum number of tokens that can be processed in a single call to llama_decode
                    ctx_params.n_batch = n_prompt;
                    // enable performance counters
                    ctx_params.no_perf = false;

                    ctx_params.n_threads = POOL_THREADS;
                    ctx_params.n_threads_batch = POOL_THREADS;

                    llama_context* ctx = llama_init_from_model(llama_model->get(), ctx_params);

                    if (ctx == NULL)
                    {
                        return error_types::MODEL_FAILED;
                    }

                    auto sparams = llama_sampler_chain_default_params();
                    sparams.no_perf = false;
                    llama_sampler* smpl = llama_sampler_chain_init(sparams);

                    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

                    // print the prompt token-by-token
                    std::string out;

                    for (auto id : prompt_tokens)
                    {
                        char buf[128];
                        int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
                        if (n < 0)
                        {
                            return error_types::UNABLE_TO_GET_PIECE;
                        }

                        out += std::string(buf, n);
                    }

                    // prepare a batch for the prompt
                    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
                    llama_token new_token_id;

                    for (int n_pos = 0; n_pos + batch.n_tokens < n_prompt + n_predict;)
                    {
                        // evaluate the current batch with the transformer model
                        if (llama_decode(ctx, batch))
                        {
                            return error_types::UNABLE_TO_DECODE;
                        }

                        n_pos += batch.n_tokens;

                        // sample the next token
                        {
                            new_token_id = llama_sampler_sample(smpl, ctx, -1);

                            // is it an end of generation?
                            if (llama_vocab_is_eog(vocab, new_token_id))
                            {
                                break;
                            }

                            char buf[128];
                            int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
                            if (n < 0)
                            {
                                return error_types::UNABLE_TO_GET_PIECE;
                            }
                            out += std::string(buf, n);

                            // prepare the next batch with the sampled token
                            batch = llama_batch_get_one(&new_token_id, 1);
                        }
                    }

                    output = std::string((uint8_t*)out.data(), (uint8_t*)out.data() + out.size());

                    llama_sampler_free(smpl);
                    llama_free(ctx);
                    return error_types::OK;
                }
                catch (const std::exception& e)
                {
                    RPC_ERROR("llamacpp exception thrown");
                    return error_types::EXCEPTION_THROWN;
                }
            }
        };

        error_types create_llama_cpp(std::shared_ptr<secret_llama::v1_0::llm_engine>& llmengine)
        {
#ifdef _DEBUG
            llama_log_set(
                [](enum ggml_log_level level, const char* text, void* /* user_data */)
                {
#ifndef _IN_ENCLAVE
                    puts(text);
#else
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
                        RPC_WARN("{}", text);
                        break;
                    case GGML_LOG_LEVEL_ERROR:
                        RPC_ERROR("{}", text);
                        break;
                    case GGML_LOG_LEVEL_NONE:
                    default:
                        break;
                    }
#endif
                },
                nullptr);
#endif

            llama_backend_init();
            ggml_backend_cpu_init();

            llmengine = std::make_shared<llama_cpp_engine>();
            return error_types::OK;
        }

        error_types parse_model(const llm_model& modl, void* data, uint64_t size, std::shared_ptr<loaded_model>& loaded_model)
        {
            auto engine = std::make_shared<llama_cpp_engine>();
            return engine->parse_model(modl, data, size, loaded_model);
        }
    } // namespace v1_0
} // namespace secret_llama
