// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <random>

#include <rpc/rpc.h>

#include "websocket_demo/websocket_demo.h"
#include "secret_llama/secret_llama.h"

// #include "secret_llama/llm_engine.h"
#include "llama_server_engine.h"

namespace secret_llama
{
    inline namespace v1_0
    {
        error_types create_llama_cpp(std::shared_ptr<secret_llama::v1_0::llm_engine>& engine);
    }
}

namespace websocket_demo
{
    namespace v1
    {
        class demo : public rpc::base<demo, v1::i_calculator>
        {
            std::shared_ptr<secret_llama::v1_0::context> context_;
            rpc::shared_ptr<i_context_event> event_;

            std::shared_ptr<rpc::service> service_;
            std::shared_ptr<bool> signal_stop_;
            std::shared_ptr<bool> complete_;
            std::shared_ptr<rpc::event> evt_stopped_;

        public:
            demo(const std::shared_ptr<rpc::service>& service, const std::shared_ptr<secret_llama::v1_0::context>& context)
                : context_(context)
                , service_(service)
            {
                signal_stop_ = std::make_shared<bool>(false);
                complete_ = std::make_shared<bool>(true);
                evt_stopped_ = std::make_shared<rpc::event>();
            }

            ~demo() override = default;

            CORO_TASK(int) add(double first_val, double second_val, double& response) override
            {
                response = first_val + second_val;
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(int) subtract(double first_val, double second_val, double& response) override
            {
                response = first_val - second_val;
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(int) multiply(double first_val, double second_val, double& response) override
            {
                response = first_val * second_val;
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(int) divide(double first_val, double second_val, double& response) override
            {
                response = first_val / second_val;
                CO_RETURN rpc::error::OK();
            }

            static CORO_TASK(void) get_next(std::shared_ptr<secret_llama::v1_0::context> context,
                rpc::shared_ptr<i_context_event> event,
                std::shared_ptr<rpc::event> evt_stopped,
                std::shared_ptr<bool> complete,
                std::shared_ptr<bool> signal_stop)
            {
                do
                {
                    std::string piece;
                    int err = context->get_piece(piece, *complete);
                    if (err != rpc::error::OK())
                    {
                        RPC_ERROR("get_piece failed {}", err);
                        CO_RETURN;
                    }
                    co_await event->piece(piece);
                } while (!*signal_stop && !*complete);
                *complete = false;
                evt_stopped->set();
            }

            CORO_TASK(int) add_prompt(const std::string& prompt) override
            {
                if (!event_)
                {
                    CO_RETURN secret_llama::v1_0::to_standard_return_type(
                        secret_llama::v1_0::error_types::CALLBACK_NOT_ASSIGNED);
                }

                if (*complete_ == false)
                {
                    *signal_stop_ = true;
                    co_await evt_stopped_->wait();
                    *signal_stop_ = false;
                    *complete_ = false;
                }

                auto err = context_->add_prompt(prompt);

                if (err != rpc::error::OK())
                {
                    co_return err;
                }

                service_->spawn(get_next(context_, event_, evt_stopped_, complete_, signal_stop_));

                CO_RETURN err;
            }

            CORO_TASK(int) set_callback(const rpc::shared_ptr<i_context_event>& event) override
            {
                if (event_)
                {
                    CO_RETURN secret_llama::v1_0::to_standard_return_type(
                        secret_llama::v1_0::error_types::CALLBACK_ALREADY_ASSIGNED);
                }

                event_ = event;

                CO_RETURN rpc::error::OK();
            }
        };

        rpc::shared_ptr<v1::i_calculator> create_websocket_demo_instance(
            const std::shared_ptr<secret_llama::v1_0::llm_engine>& engine,
            const std::shared_ptr<secret_llama::v1_0::loaded_model>& loaded_model,
            const std::shared_ptr<rpc::service> service_)
        {
            std::random_device rd;
            std::mt19937 gen(rd()); // Mersenne Twister engine
            std::uniform_int_distribution<uint64_t> dis;
            uint64_t seed = dis(gen);

            std::shared_ptr<secret_llama::v1_0::context> context;

            auto err = engine->create_context(loaded_model, seed, {}, context);
            if (secret_llama::v1_0::to_standard_return_type(err) != rpc::error::OK())
            {
                return nullptr;
            }
            return rpc::make_shared<demo>(service_, context);
        }
    }
}
