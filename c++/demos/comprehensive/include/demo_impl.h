/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Comprehensive Demo Implementations
 *   Demonstrates all major Canopy features
 */

#pragma once

#include <rpc/rpc.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>
#include <utility>

#include <comprehensive/comprehensive.h>
#include <transports/local/transport.h>

namespace comprehensive
{
    namespace v1
    {
        // ============================================================================
        // Calculator Implementation (Basic RPC)
        // ============================================================================
        class calculator_impl : public rpc::base<calculator_impl, i_calculator>,
                                public rpc::enable_shared_from_this<calculator_impl>
        {
            std::weak_ptr<rpc::service> this_service_;

        public:
            calculator_impl()
                : this_service_()
            {
            }

            calculator_impl(std::shared_ptr<rpc::service> service)
                : this_service_(service)
            {
            }

            CORO_TASK(comprehensive_error)
            add(int a,
                int b,
                int& sum) override
            {
                sum = a + b;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            subtract(
                int a,
                int b,
                int& difference) override
            {
                difference = a - b;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            multiply(
                int a,
                int b,
                int& product) override
            {
                product = a * b;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            divide(
                int a,
                int b,
                int& quotient) override
            {
                if (b == 0)
                    CO_RETURN comprehensive_error::INVALID_ARGUMENT;
                quotient = a / b;
                CO_RETURN rpc::error::OK();
            }
        };

        // ============================================================================
        // Data Processor Implementation (Serialization Demo)
        // ============================================================================
        class data_processor_impl : public rpc::base<data_processor_impl, i_data_processor>
        {
        public:
            CORO_TASK(comprehensive_error)
            process_vector(
                const std::vector<int>& input,
                std::vector<int>& output) override
            {
                output.clear();
                for (int val : input)
                {
                    output.push_back(val * 2);
                }
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            process_map(
                const std::map<
                    std::string,
                    int>& input,
                std::map<
                    std::string,
                    int>& output) override
            {
                output.clear();
                for (const auto& [key, val] : input)
                {
                    output[key] = val * 2;
                }
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            process_struct(
                const std::string& input,
                std::string& output) override
            {
                output = "Processed: " + input;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            echo_binary(
                const std::vector<uint8_t>& data,
                std::vector<uint8_t>& response) override
            {
                response = data;
                CO_RETURN rpc::error::OK();
            }
        };

        // ============================================================================
        // Callback Receiver Implementation
        // ============================================================================
        class callback_receiver_impl : public rpc::base<callback_receiver_impl, i_callback_receiver>
        {
            std::vector<int> received_progress_;
            std::vector<std::string> received_data_;
            mutable std::mutex mutex_;

        public:
            CORO_TASK(comprehensive_error) on_progress(int percentage) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                received_progress_.push_back(percentage);
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error) on_data_update(const std::string& data) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                received_data_.push_back(data);
                CO_RETURN rpc::error::OK();
            }
        };

        // ============================================================================
        // Worker Implementation (Callbacks)
        // ============================================================================
        class worker_impl : public rpc::base<worker_impl, i_worker>
        {
            rpc::shared_ptr<i_callback_receiver> parent_callback_;
            mutable std::mutex mutex_;
            bool running_{false};

        public:
            CORO_TASK(comprehensive_error) set_callback_receiver(rpc::shared_ptr<i_callback_receiver> receiver) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                parent_callback_ = receiver;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error) start_work(int iterations) override
            {
                if (iterations <= 0)
                    CO_RETURN comprehensive_error::INVALID_ARGUMENT;

                running_ = true;
                for (int i = 0; i < iterations && running_; ++i)
                {
                    // Simulate progress callback
                    if (parent_callback_)
                    {
                        auto err = CO_AWAIT parent_callback_->on_progress((i * 100) / iterations);
                        if (err == rpc::error::OBJECT_GONE())
                        {
                            // Expected: parent was released, worker continues but no callbacks
                            RPC_INFO("Parent callback returned OBJECT_GONE - continuing work without callbacks");
                        }
                        else if (err != rpc::error::OK())
                        {
                            CO_RETURN err;
                        }
                    }

                    // Simulate work
                    for (int j = 0; j < 100000; ++j)
                    {
                        // Busy work simulation
                    }
                }

                running_ = false;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error) send_data(const std::string& data) override
            {
                if (parent_callback_)
                {
                    CO_RETURN CO_AWAIT parent_callback_->on_data_update(data);
                }
                CO_RETURN rpc::error::OK();
            }
        };

        // ============================================================================
        // Managed Object Implementation (Shared Pointer Demo)
        // ============================================================================
        class managed_object_impl : public rpc::base<managed_object_impl, i_managed_object>
        {
            uint64_t object_id_;
            int ref_count_{1};
            std::string last_operation_;
            std::atomic<int> live_count_{0};

        public:
            static std::atomic<uint64_t> id_generator;

            managed_object_impl()
                : object_id_(++id_generator)
            {
                ++live_count_;
            }

            ~managed_object_impl() override { --live_count_; }

            CORO_TASK(comprehensive_error) get_object_id(uint64_t& id) override
            {
                id = object_id_;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error) get_ref_count(int& count) override
            {
                count = ref_count_;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            perform_operation(
                const std::string& operation,
                std::string& output) override
            {
                last_operation_ = operation;
                output = "Performed: " + operation + " on object " + std::to_string(object_id_);
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error) ping() override { CO_RETURN rpc::error::OK(); }
        };

        std::atomic<uint64_t> managed_object_impl::id_generator{0};

        // ============================================================================
        // Object Factory Implementation
        // ============================================================================
        class object_factory_impl : public rpc::base<object_factory_impl, i_object_factory>
        {
            std::map<uint64_t, rpc::shared_ptr<i_managed_object>> objects_;
            mutable std::mutex mutex_;

        public:
            CORO_TASK(comprehensive_error)
            create_object(
                uint64_t& object_id,
                rpc::shared_ptr<i_managed_object>& obj) override
            {
                auto new_obj = rpc::shared_ptr<i_managed_object>(new managed_object_impl());
                obj = new_obj;
                auto err = CO_AWAIT new_obj->get_object_id(object_id);
                if (err != rpc::error::OK())
                {
                    CO_RETURN err;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                objects_[object_id] = new_obj;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            get_object(
                uint64_t object_id,
                rpc::shared_ptr<i_managed_object>& obj) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = objects_.find(object_id);
                if (it != objects_.end())
                {
                    obj = it->second;
                    CO_RETURN rpc::error::OK();
                }
                CO_RETURN comprehensive_error::OBJECT_NOT_FOUND;
            }

            CORO_TASK(comprehensive_error) release_object(uint64_t object_id) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = objects_.find(object_id);
                if (it != objects_.end())
                {
                    objects_.erase(it);
                    CO_RETURN rpc::error::OK();
                }
                CO_RETURN comprehensive_error::OBJECT_NOT_FOUND;
            }
        };

        // ============================================================================
        // Demo Service Implementation (For transport demos)
        // ============================================================================
        class demo_service_impl : public rpc::base<demo_service_impl, i_demo_service>,
                                  public rpc::enable_shared_from_this<demo_service_impl>
        {
            std::string name_;
            rpc::shared_ptr<i_demo_service> child_service_;
            std::weak_ptr<rpc::service> this_service_;

        public:
            demo_service_impl(
                std::string name,
                std::shared_ptr<rpc::service> service)
                : name_(std::move(name))
                , this_service_(service)
            {
            }

            demo_service_impl(
                std::string name,
                const std::shared_ptr<rpc::child_service>& service)
                : name_(std::move(name))
                , this_service_(service)
            {
            }

            CORO_TASK(comprehensive_error) get_zone_id(uint64_t& zone_id) override
            {
                auto service = this_service_.lock();
                if (!service)
                    CO_RETURN comprehensive_error::SERVICE_UNAVAILABLE;
                zone_id = service->get_zone_id().get_subnet();
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error) get_name(std::string& name) override
            {
                name = name_;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            create_child_zone(rpc::shared_ptr<i_demo_service>& child_service_ptr) override
            {
                auto service = this_service_.lock();
                if (!service)
                    CO_RETURN comprehensive_error::SERVICE_UNAVAILABLE;

                auto child_transport = std::make_shared<rpc::local::child_transport>("child", service);
                child_transport->set_child_entry_point<i_demo_service, i_demo_service>(
                    [](const rpc::shared_ptr<i_demo_service>& parent,
                        const std::shared_ptr<rpc::child_service>& child_service_ptr)
                        -> CORO_TASK(rpc::service_connect_result<i_demo_service>)
                    {
                        CO_RETURN rpc::service_connect_result<i_demo_service>{rpc::error::OK(),
                            rpc::shared_ptr<i_demo_service>(
                                new demo_service_impl(child_service_ptr->get_name(), child_service_ptr))};
                    });

                auto connect_result = CO_AWAIT service->connect_to_zone<i_demo_service, i_demo_service>(
                    name_ + "_child_" + std::to_string(child_transport->get_adjacent_zone_id().get_subnet()),
                    child_transport,
                    rpc::static_pointer_cast<i_demo_service>(shared_from_this()));
                child_service_ptr = std::move(connect_result.output_interface);
                auto ret = connect_result.error_code;

                CO_RETURN ret;
            }

            CORO_TASK(comprehensive_error)
            echo_through_child(
                const std::string& message,
                std::string& response) override
            {
                if (!child_service_)
                    CO_RETURN comprehensive_error::OBJECT_NOT_FOUND;

                std::string child_response;
                auto err = CO_AWAIT child_service_->get_name(child_response);
                if (err != rpc::error::OK())
                {
                    response = "Error getting child name: " + std::to_string(static_cast<int>(err));
                    CO_RETURN err;
                }

                response = "Child '" + child_response + "' received: " + message;
                CO_RETURN rpc::error::OK();
            }
        };
    }
}

// Helper functions for creating implementations
namespace comprehensive
{
    namespace v1
    {
        inline rpc::shared_ptr<i_calculator> create_calculator()
        {
            return rpc::shared_ptr<i_calculator>(new calculator_impl());
        }

        inline rpc::shared_ptr<i_data_processor> create_data_processor()
        {
            return rpc::shared_ptr<i_data_processor>(new data_processor_impl());
        }

        inline rpc::shared_ptr<i_callback_receiver> create_callback_receiver()
        {
            return rpc::shared_ptr<i_callback_receiver>(new callback_receiver_impl());
        }

        inline rpc::shared_ptr<i_worker> create_worker()
        {
            return rpc::shared_ptr<i_worker>(new worker_impl());
        }

        inline rpc::shared_ptr<i_object_factory> create_object_factory()
        {
            return rpc::shared_ptr<i_object_factory>(new object_factory_impl());
        }

        inline rpc::shared_ptr<i_demo_service> create_demo_service(
            std::string name,
            std::shared_ptr<rpc::service> service)
        {
            return rpc::shared_ptr<i_demo_service>(new demo_service_impl(std::move(name), service));
        }

        inline rpc::shared_ptr<i_demo_service> create_demo_service(
            std::string name,
            const std::shared_ptr<rpc::child_service>& service)
        {
            return rpc::shared_ptr<i_demo_service>(new demo_service_impl(std::move(name), service));
        }
    }
}
