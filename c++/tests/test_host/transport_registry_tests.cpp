/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <rpc/rpc.h>

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
        return rpc::root_service::create(name.c_str(), local_zone, scheduler);
#else
        return rpc::root_service::create(name.c_str(), local_zone);
#endif
    }

    class registry_test_service final : public rpc::root_service
    {
    public:
#ifdef CANOPY_BUILD_COROUTINE
        registry_test_service(
            const char* name,
            rpc::zone zone_id,
            const std::shared_ptr<coro::scheduler>& scheduler)
            : rpc::root_service(
                  name,
                  zone_id,
                  scheduler)
        {
        }
#else
        registry_test_service(
            const char* name,
            rpc::zone zone_id)
            : rpc::root_service(
                  name,
                  zone_id)
        {
        }
#endif

        std::shared_ptr<rpc::service_proxy> add_zone_proxy_for_test(const std::shared_ptr<rpc::service_proxy>& proxy)
        {
            return add_zone_proxy(proxy);
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
        return std::make_shared<registry_test_service>(name.c_str(), local_zone, scheduler);
#else
        return std::make_shared<registry_test_service>(name.c_str(), local_zone);
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
            std::ignore = stub;
            std::ignore = input_descr;
            CO_RETURN rpc::connect_result{rpc::error::OK(), {}};
        }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

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

        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override
        {
            ++outbound_add_ref_count;
            last_add_ref_remote_object = params.remote_object_id;
            last_add_ref_caller_zone = params.caller_zone_id;
            last_add_ref_requesting_zone = params.requesting_zone_id;
            last_add_ref_options = params.build_out_param_channel;
            CO_RETURN rpc::standard_result{rpc::error::OK(), {}};
        }

        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override
        {
            std::ignore = params;
            CO_RETURN rpc::standard_result{rpc::error::OK(), {}};
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
        uint64_t outbound_add_ref_count{0};
        rpc::remote_object last_send_remote_object;
        rpc::caller_zone last_send_caller_zone;
        rpc::remote_object last_add_ref_remote_object;
        rpc::caller_zone last_add_ref_caller_zone;
        rpc::requesting_zone last_add_ref_requesting_zone;
        rpc::add_ref_options last_add_ref_options{rpc::add_ref_options::normal};
    };

#ifdef CANOPY_BUILD_COROUTINE
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
                rpc::byte_span(empty_payload));
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
            rpc::byte_span(empty_payload));
        return result.error_code == rpc::error::OK();
    }
#endif
} // namespace

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
