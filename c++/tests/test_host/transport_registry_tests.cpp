/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <common/foo_impl.h>
#include <rpc/rpc.h>

#include "test_host.h"

#ifdef CANOPY_BUILD_COROUTINE
#  include <atomic>
#  include <chrono>
#  include <coro/scheduler.hpp>
#endif

namespace
{
    rpc::zone_address make_local_zone_address(uint64_t subnet)
    {
        return rpc::to_zone_address(
            rpc::zone_address_args(
                rpc::default_values::version_3,
                rpc::address_type::local,
                0,
                {},
                rpc::default_values::default_subnet_size_bits,
                subnet,
                rpc::default_values::default_object_id_size_bits,
                0,
                {}));
    }

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> make_test_scheduler()
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{
                    .thread_count = 1,
                }}));
    }
#endif

    std::shared_ptr<rpc::root_service> make_test_service(
        const std::string& name
#ifdef CANOPY_BUILD_COROUTINE
        ,
        const std::shared_ptr<coro::scheduler>& scheduler
#endif
    )
    {
        auto local_zone = rpc::zone{make_local_zone_address(1)};
#ifdef CANOPY_BUILD_COROUTINE
        return rpc::root_service::create(name, local_zone, scheduler);
#else
        return rpc::root_service::create(name, local_zone);
#endif
    }

    class registry_test_service final : public rpc::root_service
    {
    public:
#ifdef CANOPY_BUILD_COROUTINE
        registry_test_service(
            std::string name,
            rpc::zone zone_id,
            const std::shared_ptr<coro::scheduler>& scheduler)
            : rpc::root_service(
                  std::move(name),
                  zone_id,
                  scheduler)
        {
        }
#else
        registry_test_service(
            std::string name,
            rpc::zone zone_id)
            : rpc::root_service(
                  std::move(name),
                  zone_id)
        {
        }
#endif

        std::shared_ptr<rpc::service_proxy> add_zone_proxy_for_test(const std::shared_ptr<rpc::service_proxy>& proxy)
        {
            return add_zone_proxy(proxy);
        }
    };

    class bind_failure_registry_service final : public rpc::root_service
    {
    public:
#ifdef CANOPY_BUILD_COROUTINE
        bind_failure_registry_service(
            std::string name,
            rpc::zone zone_id,
            const std::shared_ptr<coro::scheduler>& scheduler)
            : rpc::root_service(
                  std::move(name),
                  zone_id,
                  scheduler)
        {
        }
#else
        bind_failure_registry_service(
            std::string name,
            rpc::zone zone_id)
            : rpc::root_service(
                  std::move(name),
                  zone_id)
        {
        }
#endif

    private:
        std::shared_ptr<rpc::service_proxy> get_zone_proxy(
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            bool& new_proxy_added) override
        {
            std::ignore = caller_zone_id;
            std::ignore = destination_zone_id;
            new_proxy_added = false;
            return nullptr;
        }
    };

    std::shared_ptr<registry_test_service> make_registry_test_service(
        const std::string& name
#ifdef CANOPY_BUILD_COROUTINE
        ,
        const std::shared_ptr<coro::scheduler>& scheduler
#endif
    )
    {
        auto local_zone = rpc::zone{make_local_zone_address(1)};
#ifdef CANOPY_BUILD_COROUTINE
        return std::make_shared<registry_test_service>(name, local_zone, scheduler);
#else
        return std::make_shared<registry_test_service>(name, local_zone);
#endif
    }

    std::shared_ptr<bind_failure_registry_service> make_bind_failure_registry_service(
        const std::string& name
#ifdef CANOPY_BUILD_COROUTINE
        ,
        const std::shared_ptr<coro::scheduler>& scheduler
#endif
    )
    {
        auto local_zone = rpc::zone{make_local_zone_address(1)};
#ifdef CANOPY_BUILD_COROUTINE
        return std::make_shared<bind_failure_registry_service>(name, local_zone, scheduler);
#else
        return std::make_shared<bind_failure_registry_service>(name, local_zone);
#endif
    }

    class registry_test_transport final : public rpc::transport
    {
    public:
        registry_test_transport(
            std::string name,
            std::shared_ptr<rpc::service> service)
            : rpc::transport(
                  std::move(name),
                  std::move(service))
        {
            set_status(rpc::transport_status::CONNECTED);
        }

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override
        {
            ++inner_connect_count;
            std::ignore = input_descr;
            last_inner_connect_stub = stub;

            if (register_adjacent_route_in_inner_connect)
            {
                auto service = get_service();
                if (!service)
                    CO_RETURN rpc::connect_result{rpc::error::ZONE_NOT_INITIALISED(), {}};
                service->add_transport(get_adjacent_zone_id(), shared_from_this());
            }

            for (uint32_t ref_index = 0; ref_index < inner_connect_stub_refs_to_add; ++ref_index)
            {
                if (!stub)
                    CO_RETURN rpc::connect_result{rpc::error::INVALID_DATA(), {}};
                auto add_ref_result = CO_AWAIT stub->add_ref(false, false, get_adjacent_zone_id());
                if (add_ref_result != rpc::error::OK())
                    CO_RETURN rpc::connect_result{add_ref_result, {}};
            }

            CO_RETURN rpc::connect_result{inner_connect_error, inner_connect_output_descriptor};
        }

        CORO_TASK(int) inner_accept() override
        {
            ++inner_accept_count;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(rpc::send_result) outbound_send(rpc::send_params params) override
        {
            ++outbound_send_count;
            last_send_remote_object = params.remote_object_id;
            last_send_caller_zone = params.caller_zone_id;
            CO_RETURN rpc::send_result{rpc::error::OK(), {}, {}};
        }

        CORO_TASK(void) outbound_post(rpc::post_params params) override
        {
            std::ignore = params;
            CO_RETURN;
        }

        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override
        {
            std::ignore = params;
            CO_RETURN rpc::standard_result{rpc::error::OK(), {}};
        }

        CORO_TASK(rpc::get_schema_result) outbound_get_schema(rpc::get_schema_params params) override
        {
            ++outbound_get_schema_count;
            last_get_schema_destination_zone = params.destination_zone_id;

            rpc::interface_descriptor descriptor;
            descriptor.interface_id = rpc::interface_ordinal{17};
            descriptor.qualified_name = "registry_test_transport::i_schema";
            descriptor.deprecated = false;
            descriptor.schema = R"({"title":"registry schema"})";

            CO_RETURN rpc::get_schema_result{outbound_get_schema_error,
                rpc::encoding::yas_json,
                std::vector<rpc::interface_descriptor>{descriptor},
                std::vector<rpc::back_channel_entry>{rpc::back_channel_entry{7, {1, 2, 3}}}};
        }

        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override
        {
            ++outbound_add_ref_count;
            last_add_ref_remote_object = params.remote_object_id;
            last_add_ref_caller_zone = params.caller_zone_id;
            last_add_ref_requesting_zone = params.requesting_zone_id;
            last_add_ref_options = params.build_out_param_channel;
            CO_RETURN rpc::standard_result{outbound_add_ref_error, {}};
        }

        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override
        {
            ++outbound_release_count;
            last_release_remote_object = params.remote_object_id;
            last_release_caller_zone = params.caller_zone_id;
            last_release_options = params.options;
            CO_RETURN rpc::standard_result{outbound_release_error, {}};
        }

        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override
        {
            std::ignore = params;
            CO_RETURN;
        }

        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override
        {
            std::ignore = params;
            CO_RETURN;
        }

        uint64_t outbound_send_count{0};
        uint64_t outbound_get_schema_count{0};
        uint64_t outbound_add_ref_count{0};
        uint64_t outbound_release_count{0};
        int outbound_get_schema_error{rpc::error::OK()};
        int outbound_add_ref_error{rpc::error::OK()};
        int outbound_release_error{rpc::error::OK()};
        bool register_adjacent_route_in_inner_connect{false};
        uint32_t inner_connect_stub_refs_to_add{0};
        uint32_t inner_connect_count{0};
        uint32_t inner_accept_count{0};
        int inner_connect_error{rpc::error::OK()};
        rpc::remote_object inner_connect_output_descriptor{};
        std::weak_ptr<rpc::object_stub> last_inner_connect_stub;
        rpc::remote_object last_send_remote_object;
        rpc::caller_zone last_send_caller_zone;
        rpc::destination_zone last_get_schema_destination_zone;
        rpc::remote_object last_add_ref_remote_object;
        rpc::caller_zone last_add_ref_caller_zone;
        rpc::requesting_zone last_add_ref_requesting_zone;
        rpc::add_ref_options last_add_ref_options{rpc::add_ref_options::normal};
        rpc::remote_object last_release_remote_object;
        rpc::caller_zone last_release_caller_zone;
        rpc::release_options last_release_options{rpc::release_options::normal};
    };

#ifdef CANOPY_BUILD_COROUTINE
    rpc::connect_result run_transport_connect_for_test(
        const std::shared_ptr<registry_test_transport>& transport,
        rpc::connection_settings settings,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        rpc::connect_result result{rpc::error::CALL_TIMEOUT(), {}};

        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT transport->connect({}, std::move(settings));
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return result;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return result;
    }

    int run_transport_accept_for_test(
        const std::shared_ptr<registry_test_transport>& transport,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        int result = rpc::error::CALL_TIMEOUT();

        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT transport->accept();
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return result;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return result;
    }

    bool run_inbound_add_ref_for_test(
        const std::shared_ptr<rpc::transport>& transport,
        rpc::add_ref_params params,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        int error_code = rpc::error::CALL_TIMEOUT();
        auto task = [&]() -> coro::task<void>
        {
            auto result = CO_AWAIT transport->inbound_add_ref(std::move(params));
            error_code = result.error_code;
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return false;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return done.load() && error_code == rpc::error::OK();
    }

    bool run_notify_transport_down_for_test(
        const std::shared_ptr<rpc::service>& service,
        const std::shared_ptr<rpc::transport>& transport,
        rpc::destination_zone destination,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            CO_AWAIT service->notify_transport_down(transport, destination);
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return false;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return done.load();
    }

    int run_passthrough_add_ref_for_test(
        const std::shared_ptr<rpc::pass_through>& passthrough,
        rpc::add_ref_params params,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        int error_code = rpc::error::CALL_TIMEOUT();
        auto task = [&]() -> coro::task<void>
        {
            auto result = CO_AWAIT passthrough->add_ref(std::move(params));
            error_code = result.error_code;
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return rpc::error::TRANSPORT_ERROR();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return done.load() ? error_code : rpc::error::CALL_TIMEOUT();
    }

    rpc::get_schema_result run_transport_get_schema_for_test(
        const std::shared_ptr<registry_test_transport>& transport,
        rpc::get_schema_params params,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        rpc::get_schema_result result{rpc::error::CALL_TIMEOUT(), rpc::encoding::not_set, {}, {}};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT transport->get_schema(std::move(params));
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return result;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return result;
    }
#else
    bool run_inbound_add_ref_for_test(
        const std::shared_ptr<rpc::transport>& transport,
        rpc::add_ref_params params)
    {
        auto result = transport->inbound_add_ref(std::move(params));
        return result.error_code == rpc::error::OK();
    }

    bool run_notify_transport_down_for_test(
        const std::shared_ptr<rpc::service>& service,
        const std::shared_ptr<rpc::transport>& transport,
        rpc::destination_zone destination)
    {
        service->notify_transport_down(transport, destination);
        return true;
    }

    int run_passthrough_add_ref_for_test(
        const std::shared_ptr<rpc::pass_through>& passthrough,
        rpc::add_ref_params params)
    {
        return passthrough->add_ref(std::move(params)).error_code;
    }

    rpc::get_schema_result run_transport_get_schema_for_test(
        const std::shared_ptr<registry_test_transport>& transport,
        rpc::get_schema_params params)
    {
        return transport->get_schema(std::move(params));
    }
#endif

    rpc::shared_ptr<yyy::i_host> make_host_interface_for_test()
    {
        rpc::shared_ptr<yyy::i_host> local_host(new host());
        return local_host;
    }

#ifdef CANOPY_BUILD_COROUTINE
    rpc::service_connect_result<yyy::i_example> run_connect_to_zone_for_test(
        const std::shared_ptr<rpc::service>& service,
        const std::shared_ptr<registry_test_transport>& transport,
        rpc::shared_ptr<yyy::i_host> input,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        rpc::service_connect_result<yyy::i_example> result{rpc::error::CALL_TIMEOUT(), {}};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT service->connect_to_zone<yyy::i_host, yyy::i_example>(
                "transport-registry-child", transport, std::move(input));
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return result;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return result;
    }

    bool run_notify_all_destinations_for_test(
        const std::shared_ptr<registry_test_transport>& transport,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            CO_AWAIT transport->notify_all_destinations_of_disconnect();
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return false;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return done.load();
    }
#else
    rpc::connect_result run_transport_connect_for_test(
        const std::shared_ptr<registry_test_transport>& transport,
        rpc::connection_settings settings)
    {
        return transport->connect({}, std::move(settings));
    }

    int run_transport_accept_for_test(const std::shared_ptr<registry_test_transport>& transport)
    {
        return transport->accept();
    }

    rpc::service_connect_result<yyy::i_example> run_connect_to_zone_for_test(
        const std::shared_ptr<rpc::service>& service,
        const std::shared_ptr<registry_test_transport>& transport,
        rpc::shared_ptr<yyy::i_host> input)
    {
        return service->connect_to_zone<yyy::i_host, yyy::i_example>(
            "transport-registry-child", transport, std::move(input));
    }

    bool run_notify_all_destinations_for_test(const std::shared_ptr<registry_test_transport>& transport)
    {
        transport->notify_all_destinations_of_disconnect();
        return true;
    }
#endif

#ifdef CANOPY_BUILD_COROUTINE
    bool run_proxy_send_for_test(
        const std::shared_ptr<rpc::service_proxy>& proxy,
        const std::shared_ptr<coro::scheduler>& scheduler)
    {
        std::atomic_bool done{false};
        int error_code = rpc::error::CALL_TIMEOUT();
        std::vector<char> empty_payload;

        auto task = [&]() -> coro::task<void>
        {
            auto result = CO_AWAIT proxy->send_from_this_zone(
                rpc::get_version(),
                proxy->get_encoding(),
                0,
                rpc::object{1},
                rpc::interface_ordinal{1},
                rpc::method{1},
                std::move(empty_payload));
            error_code = result.error_code;
            done.store(true);
            CO_RETURN;
        };

        if (!scheduler->spawn_detached(task()))
            return false;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        return done.load() && error_code == rpc::error::OK();
    }
#else
    bool run_proxy_send_for_test(const std::shared_ptr<rpc::service_proxy>& proxy)
    {
        std::vector<char> empty_payload;
        auto result = proxy->send_from_this_zone(
            rpc::get_version(),
            proxy->get_encoding(),
            0,
            rpc::object{1},
            rpc::interface_ordinal{1},
            rpc::method{1},
            std::move(empty_payload));
        return result.error_code == rpc::error::OK();
    }
#endif
} // namespace

TEST(
    transport_lifecycle_tests,
    connect_invokes_inner_connect_once)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-connect-once", scheduler);
#else
    auto service = make_test_service("transport-connect-once");
#endif

    auto transport = std::make_shared<registry_test_transport>("connect-once-transport", service);
    rpc::connection_settings settings;
    settings.encoding_type = rpc::encoding::yas_binary;

#ifdef CANOPY_BUILD_COROUTINE
    auto first = run_transport_connect_for_test(transport, settings, scheduler);
    auto second = run_transport_connect_for_test(transport, settings, scheduler);
#else
    auto first = run_transport_connect_for_test(transport, settings);
    auto second = run_transport_connect_for_test(transport, settings);
#endif

    EXPECT_EQ(first.error_code, rpc::error::OK());
    EXPECT_EQ(second.error_code, rpc::error::INVALID_DATA());
    EXPECT_EQ(transport->inner_connect_count, 1u);
}

TEST(
    transport_lifecycle_tests,
    accept_and_connect_share_one_start_slot)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-start-once", scheduler);
#else
    auto service = make_test_service("transport-start-once");
#endif

    auto transport = std::make_shared<registry_test_transport>("start-once-transport", service);
    rpc::connection_settings settings;
    settings.encoding_type = rpc::encoding::yas_binary;

#ifdef CANOPY_BUILD_COROUTINE
    auto accept_result = run_transport_accept_for_test(transport, scheduler);
    auto connect_result = run_transport_connect_for_test(transport, settings, scheduler);
#else
    auto accept_result = run_transport_accept_for_test(transport);
    auto connect_result = run_transport_connect_for_test(transport, settings);
#endif

    EXPECT_EQ(accept_result, rpc::error::OK());
    EXPECT_EQ(connect_result.error_code, rpc::error::INVALID_DATA());
    EXPECT_EQ(transport->inner_accept_count, 1u);
    EXPECT_EQ(transport->inner_connect_count, 0u);
}

TEST(
    transport_lifecycle_tests,
    stale_disconnecting_after_disconnected_is_ignored)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-stale-disconnecting", scheduler);
#else
    auto service = make_test_service("transport-stale-disconnecting");
#endif

    auto transport = std::make_shared<registry_test_transport>("stale-disconnecting-transport", service);

    transport->set_status(rpc::transport_status::DISCONNECTING);
    EXPECT_EQ(transport->get_status(), rpc::transport_status::DISCONNECTING);

    transport->set_status(rpc::transport_status::DISCONNECTED);
    EXPECT_EQ(transport->get_status(), rpc::transport_status::DISCONNECTED);

    transport->set_status(rpc::transport_status::DISCONNECTING);
    EXPECT_EQ(transport->get_status(), rpc::transport_status::DISCONNECTED);
}

TEST(
    marshaller_params_tests,
    default_construction_is_deterministic_and_fails_closed)
{
    rpc::send_params send;
    EXPECT_EQ(send.protocol_version, 0u);
    EXPECT_EQ(send.encoding_type, rpc::encoding::not_set);
    EXPECT_EQ(send.tag, 0u);
    EXPECT_EQ(send.request_id, 0u);

    rpc::add_ref_params add_ref;
    EXPECT_EQ(add_ref.protocol_version, 0u);
    EXPECT_EQ(add_ref.build_out_param_channel, rpc::add_ref_options::normal);
    EXPECT_EQ(add_ref.request_id, 0u);

    rpc::release_params release;
    EXPECT_EQ(release.protocol_version, 0u);
    EXPECT_EQ(release.options, rpc::release_options::normal);

    rpc::standard_result standard;
    EXPECT_EQ(standard.error_code, rpc::error::NOT_INITIALISED);

    rpc::send_result send_result;
    EXPECT_EQ(send_result.error_code, rpc::error::NOT_INITIALISED);

    rpc::get_schema_result schema_result;
    EXPECT_EQ(schema_result.error_code, rpc::error::NOT_INITIALISED);
    auto plain_schema_response = schema_result.response_if_plain();
    ASSERT_NE(plain_schema_response, nullptr);
    EXPECT_EQ(plain_schema_response->encoding_type, rpc::encoding::not_set);
    EXPECT_TRUE(plain_schema_response->interfaces.empty());
}

TEST(
    transport_security_tests,
    get_schema_sanitises_positive_public_control_status)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-get-schema-status", scheduler);
#else
    auto service = make_test_service("transport-get-schema-status");
#endif

    auto transport = std::make_shared<registry_test_transport>("get-schema-status-transport", service);
    auto destination_zone = rpc::destination_zone{make_local_zone_address(81)};
    transport->set_adjacent_zone_id(destination_zone);
    transport->outbound_get_schema_error = 42;

    auto destination_object = destination_zone.with_object(rpc::object{5});
    ASSERT_TRUE(destination_object.has_value());

    rpc::get_schema_query query;
    query.remote_object_id = *destination_object;
    query.encoding_type = rpc::encoding::yas_json;

    rpc::get_schema_params params;
    params.protocol_version = rpc::get_version();
    params.caller_zone_id = service->get_zone_id();
    params.destination_zone_id = destination_zone;
    params.query = std::move(query);

#ifdef CANOPY_BUILD_COROUTINE
    auto result = run_transport_get_schema_for_test(transport, std::move(params), scheduler);
#else
    auto result = run_transport_get_schema_for_test(transport, std::move(params));
#endif

    EXPECT_EQ(result.error_code, rpc::error::PROTOCOL_ERROR());
    EXPECT_TRUE(result.out_back_channel.empty());
    auto plain_response = result.response_if_plain();
    ASSERT_NE(plain_response, nullptr);
    EXPECT_EQ(plain_response->encoding_type, rpc::encoding::not_set);
    EXPECT_TRUE(plain_response->interfaces.empty());
    EXPECT_EQ(transport->outbound_get_schema_count, 1u);
}

TEST(
    transport_registry_tests,
    initialisation_failure_releases_child_refs_to_host_objects)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-init-failure-ref-cleanup", scheduler);
#else
    auto service = make_test_service("transport-registry-init-failure-ref-cleanup");
#endif
    auto child_zone = rpc::destination_zone{make_local_zone_address(70)};

    auto transport = std::make_shared<registry_test_transport>("failing-init-transport", service);
    transport->set_adjacent_zone_id(child_zone);
    transport->register_adjacent_route_in_inner_connect = true;
    transport->inner_connect_stub_refs_to_add = 2;
    transport->inner_connect_error = rpc::error::TRANSPORT_ERROR();

    auto input = make_host_interface_for_test();
#ifdef CANOPY_BUILD_COROUTINE
    auto result = run_connect_to_zone_for_test(service, transport, input, scheduler);
#else
    auto result = run_connect_to_zone_for_test(service, transport, input);
#endif
    auto retained_stub = transport->last_inner_connect_stub;

    EXPECT_EQ(result.error_code, rpc::error::TRANSPORT_ERROR());
    input = nullptr;
    result.output_interface = nullptr;

    EXPECT_TRUE(retained_stub.expired());
    EXPECT_EQ(service->get_transport(child_zone), nullptr);
    EXPECT_EQ(transport->get_destination_count(), 0);
}

TEST(
    transport_registry_tests,
    initialisation_failure_removes_route_without_input_stub)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-init-failure-route-cleanup", scheduler);
#else
    auto service = make_test_service("transport-registry-init-failure-route-cleanup");
#endif
    auto child_zone = rpc::destination_zone{make_local_zone_address(72)};

    auto transport = std::make_shared<registry_test_transport>("failing-route-only-transport", service);
    transport->set_adjacent_zone_id(child_zone);
    transport->register_adjacent_route_in_inner_connect = true;
    transport->inner_connect_error = rpc::error::TRANSPORT_ERROR();

#ifdef CANOPY_BUILD_COROUTINE
    auto result = run_connect_to_zone_for_test(service, transport, {}, scheduler);
#else
    auto result = run_connect_to_zone_for_test(service, transport, {});
#endif

    EXPECT_EQ(result.error_code, rpc::error::TRANSPORT_ERROR());
    EXPECT_EQ(service->get_transport(child_zone), nullptr);
    EXPECT_EQ(transport->get_destination_count(), 0);
}

TEST(
    transport_registry_tests,
    output_binding_failure_removes_partial_connection_route)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_bind_failure_registry_service("transport-registry-bind-failure-route-cleanup", scheduler);
#else
    auto service = make_bind_failure_registry_service("transport-registry-bind-failure-route-cleanup");
#endif
    auto child_zone = rpc::destination_zone{make_local_zone_address(73)};
    auto foreign_zone = rpc::destination_zone{make_local_zone_address(173)};

    auto output_object = foreign_zone.with_object(rpc::object{1});
    ASSERT_TRUE(output_object);

    auto transport = std::make_shared<registry_test_transport>("failing-output-bind-transport", service);
    transport->set_adjacent_zone_id(child_zone);
    transport->register_adjacent_route_in_inner_connect = true;
    transport->inner_connect_output_descriptor = *output_object;

#ifdef CANOPY_BUILD_COROUTINE
    auto result = run_connect_to_zone_for_test(service, transport, {}, scheduler);
#else
    auto result = run_connect_to_zone_for_test(service, transport, {});
#endif

    EXPECT_EQ(result.error_code, rpc::error::ZONE_NOT_FOUND());
    EXPECT_EQ(result.output_interface, nullptr);
    EXPECT_EQ(service->get_transport(child_zone), nullptr);
    EXPECT_EQ(transport->get_destination_count(), 0);
}

TEST(
    transport_registry_tests,
    transport_down_releases_child_refs_to_host_objects_during_normal_operation)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-live-ref-cleanup", scheduler);
#else
    auto service = make_test_service("transport-registry-live-ref-cleanup");
#endif
    auto child_zone = rpc::destination_zone{make_local_zone_address(71)};

    auto transport = std::make_shared<registry_test_transport>("live-transport", service);
    transport->set_adjacent_zone_id(child_zone);
    transport->register_adjacent_route_in_inner_connect = true;
    transport->inner_connect_stub_refs_to_add = 2;

    auto input = make_host_interface_for_test();
#ifdef CANOPY_BUILD_COROUTINE
    auto result = run_connect_to_zone_for_test(service, transport, input, scheduler);
#else
    auto result = run_connect_to_zone_for_test(service, transport, input);
#endif
    ASSERT_EQ(result.error_code, rpc::error::OK());

    auto retained_stub = transport->last_inner_connect_stub;
    input = nullptr;
    result.output_interface = nullptr;

    ASSERT_FALSE(retained_stub.expired());
    EXPECT_EQ(transport->get_destination_count(), 1);
#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_TRUE(run_notify_transport_down_for_test(service, transport, child_zone, scheduler));
#else
    EXPECT_TRUE(run_notify_transport_down_for_test(service, transport, child_zone));
#endif

    EXPECT_TRUE(retained_stub.expired());
    EXPECT_EQ(service->get_transport(child_zone), nullptr);
    EXPECT_EQ(transport->get_destination_count(), 0);
}

TEST(
    transport_registry_tests,
    shutdown_notification_releases_child_refs_to_host_objects)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-shutdown-ref-cleanup", scheduler);
#else
    auto service = make_test_service("transport-registry-shutdown-ref-cleanup");
#endif
    auto child_zone = rpc::destination_zone{make_local_zone_address(72)};

    auto transport = std::make_shared<registry_test_transport>("shutdown-transport", service);
    transport->set_adjacent_zone_id(child_zone);
    transport->register_adjacent_route_in_inner_connect = true;
    transport->inner_connect_stub_refs_to_add = 2;

    auto input = make_host_interface_for_test();
#ifdef CANOPY_BUILD_COROUTINE
    auto result = run_connect_to_zone_for_test(service, transport, input, scheduler);
#else
    auto result = run_connect_to_zone_for_test(service, transport, input);
#endif
    ASSERT_EQ(result.error_code, rpc::error::OK());

    auto retained_stub = transport->last_inner_connect_stub;
    input = nullptr;
    result.output_interface = nullptr;

    ASSERT_FALSE(retained_stub.expired());
    transport->set_status(rpc::transport_status::DISCONNECTING);
#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_TRUE(run_notify_all_destinations_for_test(transport, scheduler));
#else
    EXPECT_TRUE(run_notify_all_destinations_for_test(transport));
#endif

    EXPECT_TRUE(retained_stub.expired());
    EXPECT_EQ(service->get_transport(child_zone), nullptr);
    EXPECT_EQ(transport->get_destination_count(), 0);
}

TEST(
    transport_registry_tests,
    third_direction_add_ref_uses_requesting_route_without_adjacent_identity_assumption)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-third-direction-add-ref", scheduler);
#else
    auto service = make_test_service("transport-registry-third-direction-add-ref");
#endif
    auto intermediate_zone = rpc::destination_zone{make_local_zone_address(50)};
    auto caller_zone = rpc::caller_zone{make_local_zone_address(51)};
    auto destination_zone = rpc::destination_zone{make_local_zone_address(52)};

    auto incoming_transport = std::make_shared<registry_test_transport>("incoming-from-intermediate", service);
    incoming_transport->set_adjacent_zone_id(intermediate_zone);
    EXPECT_EQ(service->add_transport(intermediate_zone, incoming_transport).get(), incoming_transport.get());

    auto remote_object = destination_zone.with_object(rpc::dummy_object_id);
    ASSERT_TRUE(remote_object.has_value());

    rpc::add_ref_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = *remote_object;
    params.caller_zone_id = caller_zone;
    params.requesting_zone_id = intermediate_zone;
    params.build_out_param_channel
        = rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route;

#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_TRUE(run_inbound_add_ref_for_test(incoming_transport, std::move(params), scheduler));
#else
    EXPECT_TRUE(run_inbound_add_ref_for_test(incoming_transport, std::move(params)));
#endif

    EXPECT_EQ(service->get_transport(destination_zone).get(), incoming_transport.get());
    EXPECT_EQ(service->get_transport(caller_zone).get(), incoming_transport.get());
    EXPECT_EQ(incoming_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(incoming_transport->last_add_ref_remote_object, *remote_object);
    EXPECT_EQ(incoming_transport->last_add_ref_caller_zone, caller_zone);
    EXPECT_EQ(incoming_transport->last_add_ref_requesting_zone, intermediate_zone);
    EXPECT_EQ(
        incoming_transport->last_add_ref_options,
        rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route);

    service->remove_transport(destination_zone);
    service->remove_transport(caller_zone);
    service->remove_transport(intermediate_zone);
}

TEST(
    transport_registry_tests,
    forked_passthrough_add_ref_compensates_committed_destination_leg)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-passthrough-add-ref-compensation", scheduler);
#else
    auto service = make_test_service("transport-registry-passthrough-add-ref-compensation");
#endif
    auto destination_zone = rpc::destination_zone{make_local_zone_address(60)};
    auto caller_zone = rpc::destination_zone{make_local_zone_address(61)};

    auto destination_transport = std::make_shared<registry_test_transport>("destination-leg", service);
    destination_transport->set_adjacent_zone_id(destination_zone);
    auto caller_transport = std::make_shared<registry_test_transport>("caller-leg", service);
    caller_transport->set_adjacent_zone_id(caller_zone);
    caller_transport->outbound_add_ref_error = rpc::error::TRANSPORT_ERROR();

    auto passthrough = rpc::transport::create_pass_through(
        destination_transport, caller_transport, service, destination_zone, caller_zone);
    ASSERT_NE(passthrough, nullptr);

    rpc::add_ref_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = rpc::remote_object(destination_zone);
    params.caller_zone_id = caller_zone;
    params.requesting_zone_id = destination_zone;
    params.build_out_param_channel
        = rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route;

#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_EQ(run_passthrough_add_ref_for_test(passthrough, std::move(params), scheduler), rpc::error::TRANSPORT_ERROR());
#else
    EXPECT_EQ(run_passthrough_add_ref_for_test(passthrough, std::move(params)), rpc::error::TRANSPORT_ERROR());
#endif

    EXPECT_EQ(destination_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(caller_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(destination_transport->outbound_release_count, 1U);
    EXPECT_EQ(caller_transport->outbound_release_count, 0U);
    EXPECT_EQ(destination_transport->last_release_remote_object, rpc::remote_object(destination_zone));
    EXPECT_EQ(destination_transport->last_release_caller_zone, caller_zone);
    EXPECT_EQ(destination_transport->last_release_options, rpc::release_options::normal);
    EXPECT_EQ(passthrough->get_shared_count(), 0U);
}

TEST(
    transport_registry_tests,
    optimistic_forked_passthrough_add_ref_compensates_with_optimistic_release)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-passthrough-optimistic-add-ref-compensation", scheduler);
#else
    auto service = make_test_service("transport-registry-passthrough-optimistic-add-ref-compensation");
#endif
    auto destination_zone = rpc::destination_zone{make_local_zone_address(62)};
    auto caller_zone = rpc::destination_zone{make_local_zone_address(63)};

    auto destination_transport = std::make_shared<registry_test_transport>("destination-leg", service);
    destination_transport->set_adjacent_zone_id(destination_zone);
    auto caller_transport = std::make_shared<registry_test_transport>("caller-leg", service);
    caller_transport->set_adjacent_zone_id(caller_zone);
    caller_transport->outbound_add_ref_error = rpc::error::TRANSPORT_ERROR();

    auto passthrough = rpc::transport::create_pass_through(
        destination_transport, caller_transport, service, destination_zone, caller_zone);
    ASSERT_NE(passthrough, nullptr);

    rpc::add_ref_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = rpc::remote_object(destination_zone);
    params.caller_zone_id = caller_zone;
    params.requesting_zone_id = destination_zone;
    params.build_out_param_channel = rpc::add_ref_options::build_destination_route
                                     | rpc::add_ref_options::build_caller_route | rpc::add_ref_options::optimistic;

#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_EQ(run_passthrough_add_ref_for_test(passthrough, std::move(params), scheduler), rpc::error::TRANSPORT_ERROR());
#else
    EXPECT_EQ(run_passthrough_add_ref_for_test(passthrough, std::move(params)), rpc::error::TRANSPORT_ERROR());
#endif

    EXPECT_EQ(destination_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(caller_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(destination_transport->outbound_release_count, 1U);
    EXPECT_EQ(destination_transport->last_release_options, rpc::release_options::optimistic);
    EXPECT_EQ(passthrough->get_optimistic_count(), 0U);
}

TEST(
    transport_registry_tests,
    forked_passthrough_add_ref_with_opaque_payload_does_not_synthesise_plain_release)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-passthrough-opaque-add-ref-compensation", scheduler);
#else
    auto service = make_test_service("transport-registry-passthrough-opaque-add-ref-compensation");
#endif
    auto destination_zone = rpc::destination_zone{make_local_zone_address(64)};
    auto caller_zone = rpc::destination_zone{make_local_zone_address(65)};

    auto destination_transport = std::make_shared<registry_test_transport>("destination-leg", service);
    destination_transport->set_adjacent_zone_id(destination_zone);
    auto caller_transport = std::make_shared<registry_test_transport>("caller-leg", service);
    caller_transport->set_adjacent_zone_id(caller_zone);
    caller_transport->outbound_add_ref_error = rpc::error::TRANSPORT_ERROR();

    auto passthrough = rpc::transport::create_pass_through(
        destination_transport, caller_transport, service, destination_zone, caller_zone);
    ASSERT_NE(passthrough, nullptr);

    rpc::add_ref_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = rpc::remote_object(destination_zone);
    params.caller_zone_id = caller_zone;
    params.requesting_zone_id = destination_zone;
    params.build_out_param_channel
        = rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route;
    params.payload = rpc::typed_payload{0x1234U, rpc::encoding::yas_binary, std::vector<char>{'p'}};

#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_EQ(run_passthrough_add_ref_for_test(passthrough, std::move(params), scheduler), rpc::error::TRANSPORT_ERROR());
#else
    EXPECT_EQ(run_passthrough_add_ref_for_test(passthrough, std::move(params)), rpc::error::TRANSPORT_ERROR());
#endif

    EXPECT_EQ(destination_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(caller_transport->outbound_add_ref_count, 1U);
    EXPECT_EQ(destination_transport->outbound_release_count, 0U);
    EXPECT_EQ(caller_transport->outbound_release_count, 0U);
    EXPECT_EQ(passthrough->get_shared_count(), 0U);
}

TEST(
    transport_registry_tests,
    duplicate_live_transport_keeps_first_registration)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-duplicate-live", scheduler);
#else
    auto service = make_test_service("transport-registry-duplicate-live");
#endif
    auto destination = rpc::destination_zone{make_local_zone_address(42)};

    auto first_transport = std::make_shared<registry_test_transport>("first", service);
    first_transport->set_adjacent_zone_id(destination);
    auto duplicate_transport = std::make_shared<registry_test_transport>("duplicate", service);
    duplicate_transport->set_adjacent_zone_id(destination);

    EXPECT_EQ(service->add_transport(destination, first_transport).get(), first_transport.get());
    EXPECT_EQ(service->add_transport(destination, duplicate_transport).get(), first_transport.get());
    EXPECT_EQ(service->get_transport(destination).get(), first_transport.get());

    service->remove_transport_if_matches(destination, duplicate_transport.get());
    EXPECT_EQ(service->get_transport(destination).get(), first_transport.get());

    service->remove_transport_if_matches(destination, first_transport.get());
    EXPECT_EQ(service->get_transport(destination), nullptr);
}

TEST(
    transport_registry_tests,
    duplicate_proxy_cleanup_keeps_canonical_proxy)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_registry_test_service("transport-registry-duplicate-proxy", scheduler);
#else
    auto service = make_registry_test_service("transport-registry-duplicate-proxy");
#endif
    auto destination = rpc::destination_zone{make_local_zone_address(44)};

    auto canonical_transport = std::make_shared<registry_test_transport>("canonical", service);
    canonical_transport->set_adjacent_zone_id(destination);
    auto duplicate_transport = std::make_shared<registry_test_transport>("duplicate", service);
    duplicate_transport->set_adjacent_zone_id(destination);

    EXPECT_EQ(service->add_transport(destination, canonical_transport).get(), canonical_transport.get());

    auto canonical_proxy = rpc::service_proxy::create("canonical-proxy", service, canonical_transport, destination);
    EXPECT_EQ(service->add_zone_proxy_for_test(canonical_proxy).get(), canonical_proxy.get());

    {
        auto duplicate_proxy = rpc::service_proxy::create("duplicate-proxy", service, duplicate_transport, destination);
        EXPECT_EQ(service->add_zone_proxy_for_test(duplicate_proxy).get(), canonical_proxy.get());
    }

    auto second_duplicate_proxy
        = rpc::service_proxy::create("second-duplicate-proxy", service, duplicate_transport, destination);
    EXPECT_EQ(service->add_zone_proxy_for_test(second_duplicate_proxy).get(), canonical_proxy.get());
}

TEST(
    transport_registry_tests,
    disconnecting_transport_can_be_replaced)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_test_service("transport-registry-disconnecting", scheduler);
#else
    auto service = make_test_service("transport-registry-disconnecting");
#endif
    auto destination = rpc::destination_zone{make_local_zone_address(43)};

    auto stale_transport = std::make_shared<registry_test_transport>("stale", service);
    stale_transport->set_adjacent_zone_id(destination);
    auto replacement_transport = std::make_shared<registry_test_transport>("replacement", service);
    replacement_transport->set_adjacent_zone_id(destination);

    EXPECT_EQ(service->add_transport(destination, stale_transport).get(), stale_transport.get());
    stale_transport->set_status(rpc::transport_status::DISCONNECTING);

    EXPECT_EQ(service->add_transport(destination, replacement_transport).get(), replacement_transport.get());
    EXPECT_EQ(service->get_transport(destination).get(), replacement_transport.get());

    service->remove_transport_if_matches(destination, replacement_transport.get());
    EXPECT_EQ(service->get_transport(destination), nullptr);
}

TEST(
    transport_registry_tests,
    stale_transport_down_does_not_remove_replacement_route)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = make_test_scheduler();
    auto service = make_registry_test_service("transport-registry-stale-down", scheduler);
#else
    auto service = make_registry_test_service("transport-registry-stale-down");
#endif
    auto destination = rpc::destination_zone{make_local_zone_address(45)};

    auto stale_transport = std::make_shared<registry_test_transport>("stale", service);
    stale_transport->set_adjacent_zone_id(destination);
    auto replacement_transport = std::make_shared<registry_test_transport>("replacement", service);
    replacement_transport->set_adjacent_zone_id(destination);

    EXPECT_EQ(service->add_transport(destination, stale_transport).get(), stale_transport.get());
    auto stale_proxy = rpc::service_proxy::create("stale-proxy", service, stale_transport, destination);
    EXPECT_EQ(service->add_zone_proxy_for_test(stale_proxy).get(), stale_proxy.get());

    stale_transport->set_status(rpc::transport_status::DISCONNECTING);
    EXPECT_EQ(service->add_transport(destination, replacement_transport).get(), replacement_transport.get());

    auto replacement_proxy = rpc::service_proxy::create("replacement-proxy", service, replacement_transport, destination);
    EXPECT_EQ(service->add_zone_proxy_for_test(replacement_proxy).get(), replacement_proxy.get());

#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_TRUE(run_notify_transport_down_for_test(service, stale_transport, destination, scheduler));
#else
    EXPECT_TRUE(run_notify_transport_down_for_test(service, stale_transport, destination));
#endif

    EXPECT_EQ(service->get_transport(destination).get(), replacement_transport.get());

    stale_proxy.reset();
    auto duplicate_proxy
        = rpc::service_proxy::create("duplicate-after-stale-down", service, replacement_transport, destination);
    auto proxy_after_stale_down = service->add_zone_proxy_for_test(duplicate_proxy);
    EXPECT_EQ(proxy_after_stale_down.get(), replacement_proxy.get());

    // This is the observable part of the repair: after the stale transport reports
    // down, the canonical route must still be usable, and any new lookup must route
    // through the replacement transport rather than the stale one.
#ifdef CANOPY_BUILD_COROUTINE
    EXPECT_TRUE(run_proxy_send_for_test(proxy_after_stale_down, scheduler));
#else
    EXPECT_TRUE(run_proxy_send_for_test(proxy_after_stale_down));
#endif
    EXPECT_EQ(stale_transport->outbound_send_count, 0U);
    EXPECT_EQ(replacement_transport->outbound_send_count, 1U);
    EXPECT_EQ(replacement_transport->last_send_remote_object.as_zone(), destination);
    EXPECT_EQ(replacement_transport->last_send_caller_zone, service->get_zone_id());
}
