// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <memory>
#include <tuple>

#include <rpc/rpc.h>

#ifndef CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
#  define CANOPY_WEBSOCKET_DEMO_HAS_LLM 1
#else
#  define CANOPY_WEBSOCKET_DEMO_HAS_LLM 0
#endif

#ifdef CANOPY_WEBSOCKET_DEMO_ENABLE_VIDEO
#  define CANOPY_WEBSOCKET_DEMO_HAS_VIDEO 1
#else
#  define CANOPY_WEBSOCKET_DEMO_HAS_VIDEO 0
#endif

#if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
#  include "video_session.h"
#endif
#include "websocket_demo/websocket_demo.h"

#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
#  include <random>

#  include "secret_llama/secret_llama.h"

// #include "secret_llama/llm_engine.h"
#  include "llama_server_engine.h"

namespace secret_llama
{
    inline namespace v1_0
    {
        error_types create_llama_cpp(std::shared_ptr<secret_llama::v1_0::llm_engine>& engine);
    }
}
#endif

namespace websocket_demo
{
    namespace v1
    {
        class demo : public rpc::base<demo, v1::i_calculator>
        {
#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
            std::shared_ptr<secret_llama::v1_0::context> context_;
#endif
            rpc::shared_ptr<i_context_event> event_;
#if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
            video_session video_;
#endif

#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
            std::shared_ptr<rpc::service> service_;
            std::shared_ptr<bool> signal_stop_;
            std::shared_ptr<bool> complete_;
            std::shared_ptr<rpc::event> evt_stopped_;
#endif

        public:
#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
            demo(
                const std::shared_ptr<rpc::service>& service,
                const std::shared_ptr<secret_llama::v1_0::context>& context)
                : context_(context)
                , service_(service)
            {
                signal_stop_ = std::make_shared<bool>(false);
                complete_ = std::make_shared<bool>(true);
                evt_stopped_ = std::make_shared<rpc::event>();
#  if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
                video_.set_scheduler(service_->get_scheduler());
#  endif
            }
#elif CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
            explicit demo(const std::shared_ptr<rpc::service>& service)
            {
                if (service)
                    video_.set_scheduler(service->get_scheduler());
            }
#else
            demo() = default;
#endif

            ~demo() override { };

            CORO_TASK(websocket_error)
            add(double first_val,
                double second_val,
                double& response) override
            {
                response = first_val + second_val;
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(websocket_error)
            subtract(
                double first_val,
                double second_val,
                double& response) override
            {
                response = first_val - second_val;
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(websocket_error)
            multiply(
                double first_val,
                double second_val,
                double& response) override
            {
                response = first_val * second_val;
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(websocket_error)
            divide(
                double first_val,
                double second_val,
                double& response) override
            {
                response = first_val / second_val;
                CO_RETURN rpc::error::OK();
            }

#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
            static CORO_TASK(void) get_next(
                std::shared_ptr<secret_llama::v1_0::context> context,
                rpc::shared_ptr<i_context_event> event,
                std::shared_ptr<rpc::event> evt_stopped,
                std::shared_ptr<bool> complete,
                std::shared_ptr<bool> signal_stop)
            {
                do
                {
                    std::string piece;
                    auto err = context->get_piece(piece, *complete);
                    if (err != rpc::error::OK())
                    {
                        RPC_ERROR("get_piece failed {}", secret_llama::v1_0::error_types::to_string(err));
                        CO_RETURN;
                    }
                    co_await event->piece(piece);
                } while (!*signal_stop && !*complete);
                *complete = false;
                evt_stopped->set();
            }

            CORO_TASK(websocket_error) add_prompt(const std::string& prompt) override
            {
                if (!event_)
                {
                    CO_RETURN websocket_error::CALLBACK_NOT_ASSIGNED;
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
                    CO_RETURN websocket_error::LLM_ENGINE_FAILURE;
                }

                service_->spawn(get_next(context_, event_, evt_stopped_, complete_, signal_stop_));

                CO_RETURN rpc::error::OK();
            }
#else
            CORO_TASK(websocket_error) add_prompt(const std::string& prompt) override
            {
                std::ignore = prompt;
                if (event_)
                    CO_AWAIT event_->piece("LLM support is not enabled in this websocket demo.");
                CO_RETURN websocket_error::FEATURE_NOT_ENABLED;
            }
#endif

            CORO_TASK(websocket_error) set_callback(const rpc::shared_ptr<i_context_event>& event) override
            {
#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
                if (event_)
                {
                    CO_RETURN websocket_error::CALLBACK_ALREADY_ASSIGNED;
                }
#endif

                event_ = event;
#if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
                video_.set_sink(event);
#endif

                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(websocket_error)
            push_video_frame(
                uint64_t seq,
                uint64_t pts_us,
                uint32_t flags,
                const std::vector<uint8_t>& payload) override
            {
#if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
                CO_RETURN CO_AWAIT video_.forward_frame(seq, pts_us, flags, payload);
#else
                (void)seq;
                (void)pts_us;
                (void)flags;
                (void)payload;
                CO_RETURN websocket_error::FEATURE_NOT_ENABLED;
#endif
            }

            CORO_TASK(websocket_error) set_video_effects(uint32_t effects) override
            {
#if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
                video_.set_effects(effects);
#else
                (void)effects;
#endif
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(websocket_error)
            set_video_params(
                int32_t brightness,
                uint32_t bitrate_kbps,
                uint32_t cpu_used) override
            {
#if CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
                video_.set_params(brightness, bitrate_kbps, cpu_used);
#else
                (void)brightness;
                (void)bitrate_kbps;
                (void)cpu_used;
#endif
                CO_RETURN rpc::error::OK();
            }
        };

#if CANOPY_WEBSOCKET_DEMO_HAS_LLM
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
            if (err != secret_llama::v1_0::error_types::OK)
            {
                return nullptr;
            }
            return rpc::make_shared<demo>(service_, context);
        }
#elif CANOPY_WEBSOCKET_DEMO_HAS_VIDEO
        rpc::shared_ptr<v1::i_calculator> create_websocket_demo_instance(const std::shared_ptr<rpc::service>& service)
        {
            return rpc::make_shared<demo>(service);
        }

        rpc::shared_ptr<v1::i_calculator> create_websocket_demo_instance()
        {
            return create_websocket_demo_instance(nullptr);
        }
#else
        rpc::shared_ptr<v1::i_calculator> create_websocket_demo_instance()
        {
            return rpc::make_shared<demo>();
        }
#endif
    }
}
