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
            std::ignore = params;
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
            std::ignore = params;
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
    };
} // namespace

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
