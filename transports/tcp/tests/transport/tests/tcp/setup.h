/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <common/foo_impl.h>

#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/i_telemetry_service.h>
#include <rpc/telemetry/multiplexing_telemetry_service.h>
#endif

#include <transports/tcp/transport.h>
#include <transports/tcp/listener.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone> class tcp_setup
{
    std::shared_ptr<rpc::service> root_service_;
    std::shared_ptr<rpc::service> peer_service_;

    std::shared_ptr<rpc::tcp::tcp_transport> server_transport_;
    std::shared_ptr<rpc::tcp::tcp_transport> client_transport_;
    std::unique_ptr<rpc::tcp::listener> listener_;
    rpc::shared_ptr<yyy::i_host> i_host_ptr_;
    rpc::weak_ptr<yyy::i_host> local_host_ptr_;
    rpc::shared_ptr<yyy::i_example> i_example_ptr_;

    const bool has_enclave_ = true;
    bool use_host_in_child_ = UseHostInChild;
    bool run_standard_tests_ = RunStandardTests;

    std::atomic<uint64_t> zone_gen_ = 0;

    std::shared_ptr<coro::io_scheduler> io_scheduler_;
    bool error_has_occured_ = false;
    bool setup_complete_ = false;

public:
    std::shared_ptr<coro::io_scheduler> get_scheduler() const { return io_scheduler_; }
    bool error_has_occured() const { return error_has_occured_; }

    virtual ~tcp_setup() = default;

    std::shared_ptr<rpc::service> get_root_service() const { return root_service_; }
    std::shared_ptr<rpc::tcp::tcp_transport> get_server_transport() const { return server_transport_; };
    bool get_has_enclave() const { return has_enclave_; }
    bool is_sgx_setup() const { return false; }
    rpc::shared_ptr<yyy::i_example> get_example() const { return i_example_ptr_; }
    void set_example(const rpc::shared_ptr<yyy::i_example>& example) { i_example_ptr_ = example; }
    rpc::shared_ptr<yyy::i_host> get_host() const { return i_host_ptr_; }
    void set_host(const rpc::shared_ptr<yyy::i_host>& host) { i_host_ptr_ = host; }
    rpc::shared_ptr<yyy::i_host> get_local_host_ptr() { return local_host_ptr_.lock(); }
    bool get_use_host_in_child() const { return use_host_in_child_; }

    CORO_TASK(void) check_for_error(CORO_TASK(bool) task)
    {
        auto ret = CO_AWAIT task;
        if (!ret)
        {
            error_has_occured_ = true;
        }
        CO_RETURN;
    }

    CORO_TASK(bool) CoroSetUp()
    {
        zone_gen = &zone_gen_;
#ifdef CANOPY_USE_TELEMETRY
        auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        if (auto telemetry_service
            = std::static_pointer_cast<rpc::multiplexing_telemetry_service>(rpc::get_telemetry_service()))
        {
            telemetry_service->start_test(test_info->test_suite_name(), test_info->name());
        }
#endif

        auto root_zone_id = rpc::zone{++zone_gen_};
        auto peer_zone_id = rpc::zone{++zone_gen_};

        // Create the peer service (server side)
        peer_service_ = std::make_shared<rpc::service>("peer", peer_zone_id, io_scheduler_);
        example_import_idl_register_stubs(peer_service_);
        example_shared_idl_register_stubs(peer_service_);
        example_idl_register_stubs(peer_service_);

        // Create the root service (client side)
        root_service_ = std::make_shared<rpc::service>("host", root_zone_id, io_scheduler_);
        example_import_idl_register_stubs(root_service_);
        example_shared_idl_register_stubs(root_service_);
        example_idl_register_stubs(root_service_);

        current_host_service = root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        local_host_ptr_ = hst; // assign to weak ptr

        // Create the listener for the server side
        // The connection handler will be called when a client connects
        listener_ = std::make_unique<rpc::tcp::listener>(
            [this, use_host_in_child = use_host_in_child_](const rpc::interface_descriptor& input_descr,
                rpc::interface_descriptor& output_interface,
                std::shared_ptr<rpc::service> child_service_ptr,
                std::shared_ptr<rpc::tcp::tcp_transport> transport) -> CORO_TASK(int)
            {
                // Server-side connection handler
                // Store the transport for later use
                server_transport_ = transport;

                // Add the transport to the service first, BEFORE calling attach_remote_zone
                // attach_remote_zone expects the transport to already be registered
                child_service_ptr->add_transport(input_descr.destination_zone_id, transport);

                // Use attach_remote_zone to properly manage object lifetime, like SPSC does
                auto ret = CO_AWAIT child_service_ptr->attach_remote_zone<yyy::i_host, yyy::i_example>("service_proxy",
                    transport,
                    input_descr,
                    output_interface,
                    [&](const rpc::shared_ptr<yyy::i_host>& host,
                        rpc::shared_ptr<yyy::i_example>& new_example,
                        const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
                    {
                        new_example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(service_ptr, host));

                        if (use_host_in_child)
                            CO_AWAIT new_example->set_host(host);
                        CO_RETURN rpc::error::OK();
                    });
                CO_RETURN ret;
            },
            std::chrono::milliseconds(100000));

        // Start the listener on the peer service
        auto server_options = coro::net::tcp::server::options{
            .address = {coro::net::ip_address::from_string("127.0.0.1")}, .port = 8080, .backlog = 128};

        if (!listener_->start_listening(peer_service_, server_options))
        {
            RPC_ERROR("Failed to start TCP listener");
            CO_RETURN false;
        }

        // Create a TCP client and connect to the server
        auto scheduler = root_service_->get_scheduler();
        coro::net::tcp::client client(scheduler,
            coro::net::tcp::client::options{
                .address = {coro::net::ip_address::from_string("127.0.0.1")},
                .port = 8080,
            });

        auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
        if (connection_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("Failed to connect TCP client to server (status: {})", static_cast<int>(connection_status));
            CO_RETURN false;
        }

        // Create the client transport
        client_transport_ = rpc::tcp::tcp_transport::create("client_transport",
            root_service_,
            peer_zone_id,
            std::chrono::milliseconds(100000),
            std::move(client),
            nullptr); // client doesn't need handler

        // Start the client pump - this must run before we call connect
        root_service_->spawn(client_transport_->pump_send_and_receive());

        // Connect using the client transport
        auto ret = CO_AWAIT root_service_->connect_to_zone("main child", client_transport_, hst, i_example_ptr_);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect to zone: {}", ret);
            CO_RETURN false;
        }

        CO_RETURN true;
    }

    virtual void set_up()
    {
        setup_complete_ = false;
        io_scheduler_ = coro::io_scheduler::make_shared(
            coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{
                    .thread_count = 1,
                }});

        auto setup_task = [this]() -> coro::task<void>
        {
            CO_AWAIT check_for_error(CoroSetUp());
            setup_complete_ = true;
            CO_RETURN;
        };

        io_scheduler_->spawn(setup_task());

        // Process events until setup completes
        // Note: pump_send_and_receive tasks keep running, so scheduler never empties
        while (!setup_complete_)
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        ASSERT_EQ(error_has_occured_, false);
    }

    CORO_TASK(void) CoroTearDown()
    {
        i_example_ptr_ = nullptr;
        i_host_ptr_ = nullptr;
        local_host_ptr_.reset();

        // Stop the listener first
        if (listener_)
        {
            CO_AWAIT listener_->stop_listening();
            listener_.reset();
        }

        CO_RETURN;
    }

    virtual void tear_down()
    {
        bool shutdown_complete = false;
        auto shutdown_task = [&]() -> coro::task<void>
        {
            CO_AWAIT CoroTearDown();
            // Give time for transport destructors to schedule detach coroutines
            CO_AWAIT io_scheduler_->schedule();
            CO_AWAIT io_scheduler_->schedule();
            shutdown_complete = true;
            CO_RETURN;
        };

        RPC_ASSERT(io_scheduler_->spawn(shutdown_task()));

        // Process events until shutdown completes
        while (!shutdown_complete)
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        // Continue processing to allow shutdown to finish
        for (int i = 0; i < 100; ++i)
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        peer_service_.reset();
        root_service_.reset();
        zone_gen = nullptr;
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service
            = std::static_pointer_cast<rpc::multiplexing_telemetry_service>(rpc::get_telemetry_service()))
        {
            telemetry_service->reset_for_test();
        }
#endif
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        // TCP setup doesn't support creating local child zones via connect_to_zone
        // Instead, TCP is for connecting to remote services over network
        // For local child zones, use inproc_setup instead
        RPC_ERROR("create_new_zone is not implemented for tcp_setup - use inproc_setup for local child zones");
        CO_RETURN nullptr;
    }
};
