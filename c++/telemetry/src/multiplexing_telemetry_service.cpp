/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/telemetry/telemetry_service_factory.h>

#include <vector>
#include <memory>
#include <string>
#include <filesystem>

#include <rpc/rpc.h>
#include <rpc/telemetry/i_telemetry_service.h>

#ifndef FOR_SGX
// Forward declare TestInfo to avoid including gtest in headers
namespace testing
{
    class TestInfo;
}
#endif

namespace rpc::telemetry
{
#ifndef FOR_SGX
    /**
     * @brief Configuration for creating telemetry services with test-specific names.
     */
    struct telemetry_service_config
    {
        std::string type;        // "console", "file", etc.
        std::string output_path; // For file/console services that need a path

        telemetry_service_config(
            const std::string& t,
            const std::string& path = "")
            : type(t)
            , output_path(path)
        {
        }
    };
#endif

    /**
     * @brief A telemetry service that forwards all telemetry events to multiple child services.
     *
     * This service implements i_telemetry_service and acts as a multiplexer, forwarding all
     * telemetry events to a configurable list of child telemetry services. This allows
     * running multiple telemetry backends simultaneously (e.g., console + file + custom).
     *
     * Non-enclave builds only.
     */
    class multiplexing_telemetry_service : public i_telemetry_service
    {
    private:
        std::vector<std::shared_ptr<i_telemetry_service>> children_;
#ifndef FOR_SGX
        std::vector<telemetry_service_config> service_configs_;
#endif

    public:
        /**
         * @brief Factory method to create a multiplexing telemetry service.
         *
         * @param service Output parameter for the created service
         * @param child_services Vector of child telemetry services to forward to
         * @return true if creation was successful, false otherwise
         */
        static bool create(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services);

        /**
         * @brief Constructor with child services.
         *
         * @param child_services Vector of child telemetry services to forward to
         */
        explicit multiplexing_telemetry_service(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services);

        ~multiplexing_telemetry_service() override CANOPY_DEFAULT_DESTRUCTOR;

        multiplexing_telemetry_service(const multiplexing_telemetry_service&) = delete;
        multiplexing_telemetry_service& operator=(const multiplexing_telemetry_service&) = delete;

        /**
         * @brief Add a child telemetry service to forward events to.
         *
         * @param child The child service to add
         */
        void add_child(std::shared_ptr<i_telemetry_service> child);

        /**
         * @brief Get the number of child services.
         *
         * @return size_t Number of child services
         */
        [[nodiscard]] size_t get_child_count() const;

        /**
         * @brief Clear all child services (for test cleanup).
         */
        void clear_children();

        void handle_telemetry_event(rpc::telemetry_event event) const override;

#ifndef FOR_SGX
        /**
         * @brief Register a telemetry service configuration that will be created for each test.
         *
         * @param type Type of service ("console", "sequence", "animation", etc.)
         * @param output_path Optional output path for services that need it
         */
        void register_service_config(
            const std::string& type,
            const std::string& output_path = "");

        /**
         * @brief Reset for a new test - clear children and recreate them with current test info.
         *
         * @param test_info Current test information for creating services with correct names
         */
        void start_test(
            const std::string& test_suite_name,
            const std::string& name);

        /**
         * @brief Reset for a new test - clear children hem with current test info.
         */
        void reset_for_test();
#endif

        // i_telemetry_service interface - all methods forward to children
        void on_service_creation(const rpc::telemetry::service_creation_event& event) const override;
        void on_service_deletion(const rpc::telemetry::service_deletion_event& event) const override;
        void on_service_send(const rpc::telemetry::service_send_event& event) const override;
        void on_service_post(const rpc::telemetry::service_post_event& event) const override;
        void on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const override;
        void on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const override;
        void on_service_release(const rpc::telemetry::service_release_event& event) const override;
        void on_service_object_released(const rpc::telemetry::service_object_released_event& event) const override;
        void on_service_transport_down(const rpc::telemetry::service_transport_down_event& event) const override;
        void on_service_proxy_creation(const rpc::telemetry::service_proxy_creation_event& event) const override;
        void on_cloned_service_proxy_creation(
            const rpc::telemetry::cloned_service_proxy_creation_event& event) const override;
        void on_service_proxy_deletion(const rpc::telemetry::service_proxy_deletion_event& event) const override;
        void on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const override;
        void on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const override;
        void on_service_proxy_try_cast(const rpc::telemetry::service_proxy_try_cast_event& event) const override;
        void on_service_proxy_add_ref(const rpc::telemetry::service_proxy_add_ref_event& event) const override;
        void on_service_proxy_release(const rpc::telemetry::service_proxy_release_event& event) const override;
        void on_service_proxy_object_released(
            const rpc::telemetry::service_proxy_object_released_event& event) const override;
        void on_service_proxy_transport_down(const rpc::telemetry::service_proxy_transport_down_event& event) const override;
        void on_service_proxy_add_external_ref(const rpc::telemetry::service_proxy_external_ref_event& event) const override;
        void on_service_proxy_release_external_ref(
            const rpc::telemetry::service_proxy_external_ref_event& event) const override;
        void on_transport_creation(const rpc::telemetry::transport_creation_event& event) const override;
        void on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const override;
        void on_transport_status_change(const rpc::telemetry::transport_status_change_event& event) const override;
        void on_transport_add_destination(const rpc::telemetry::transport_destination_event& event) const override;
        void on_transport_remove_destination(const rpc::telemetry::transport_destination_event& event) const override;
        void on_transport_accept(const rpc::telemetry::transport_accept_event& event) const override;
        void on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const override;
        void on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const override;
        void on_transport_outbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const override;
        void on_transport_outbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const override;
        void on_transport_outbound_release(const rpc::telemetry::transport_release_event& event) const override;
        void on_transport_outbound_object_released(
            const rpc::telemetry::transport_object_released_event& event) const override;
        void on_transport_outbound_transport_down(const rpc::telemetry::transport_transport_down_event& event) const override;
        void on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const override;
        void on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const override;
        void on_transport_inbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const override;
        void on_transport_inbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const override;
        void on_transport_inbound_release(const rpc::telemetry::transport_release_event& event) const override;
        void on_transport_inbound_object_released(
            const rpc::telemetry::transport_object_released_event& event) const override;
        void on_transport_inbound_transport_down(const rpc::telemetry::transport_transport_down_event& event) const override;
        void on_impl_creation(const rpc::telemetry::impl_creation_event& event) const override;
        void on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const override;
        void on_stub_creation(const rpc::telemetry::stub_creation_event& event) const override;
        void on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const override;
        void on_stub_send(const rpc::telemetry::stub_send_event& event) const override;
        void on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const override;
        void on_stub_release(const rpc::telemetry::stub_release_event& event) const override;
        void on_object_proxy_creation(const rpc::telemetry::object_proxy_creation_event& event) const override;
        void on_object_proxy_deletion(const rpc::telemetry::object_proxy_deletion_event& event) const override;
        void on_interface_proxy_creation(const rpc::telemetry::interface_proxy_creation_event& event) const override;
        void on_interface_proxy_deletion(const rpc::telemetry::interface_proxy_deletion_event& event) const override;
        void on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const override;
        void on_pass_through_creation(const rpc::telemetry::pass_through_creation_event& event) const override;
        void on_pass_through_deletion(const rpc::telemetry::pass_through_deletion_event& event) const override;
        void on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const override;
        void on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const override;
        void on_pass_through_status_change(const rpc::telemetry::pass_through_status_change_event& event) const override;
        void message(const rpc::log_record& event) const override;
    };
}

namespace rpc::telemetry
{
    bool multiplexing_telemetry_service::create(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services)
    {
        // Allow empty multiplexer - services can be added later
        rpc::telemetry::telemetry_service_ = std::make_shared<multiplexing_telemetry_service>(std::move(child_services));
        return true;
    }

    bool create_multiplexing_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        std::vector<std::shared_ptr<i_telemetry_service>>&& child_services)
    {
        service = std::make_shared<multiplexing_telemetry_service>(std::move(child_services));
        return true;
    }

    bool create_global_multiplexing_telemetry_service(std::vector<std::shared_ptr<i_telemetry_service>>&& child_services)
    {
        return create_multiplexing_telemetry_service(telemetry_service_, std::move(child_services));
    }

    bool add_telemetry_child(
        const std::shared_ptr<i_telemetry_service>& service,
        std::shared_ptr<i_telemetry_service> child)
    {
        auto multiplexing_service = std::dynamic_pointer_cast<multiplexing_telemetry_service>(service);
        if (!multiplexing_service)
            return false;
        multiplexing_service->add_child(std::move(child));
        return true;
    }

    bool register_telemetry_service_config(
        const std::shared_ptr<i_telemetry_service>& service,
        const std::string& type,
        const std::string& output_path)
    {
        auto multiplexing_service = std::dynamic_pointer_cast<multiplexing_telemetry_service>(service);
        if (!multiplexing_service)
            return false;
        multiplexing_service->register_service_config(type, output_path);
        return true;
    }

    bool start_telemetry_test(
        const std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name)
    {
        auto multiplexing_service = std::dynamic_pointer_cast<multiplexing_telemetry_service>(service);
        if (!multiplexing_service)
            return false;
        multiplexing_service->start_test(test_suite_name, name);
        return true;
    }

    bool reset_telemetry_for_test(const std::shared_ptr<i_telemetry_service>& service)
    {
        auto multiplexing_service = std::dynamic_pointer_cast<multiplexing_telemetry_service>(service);
        if (!multiplexing_service)
            return false;
        multiplexing_service->reset_for_test();
        return true;
    }

    multiplexing_telemetry_service::multiplexing_telemetry_service(
        std::vector<std::shared_ptr<i_telemetry_service>>&& child_services)
        : children_(std::move(child_services))
    {
    }

    void multiplexing_telemetry_service::add_child(std::shared_ptr<i_telemetry_service> child)
    {
        if (child)
        {
            children_.push_back(child);
        }
    }

    size_t multiplexing_telemetry_service::get_child_count() const
    {
        return children_.size();
    }

    void multiplexing_telemetry_service::clear_children()
    {
        children_.clear();
    }

#ifndef FOR_SGX
    void multiplexing_telemetry_service::register_service_config(
        const std::string& type,
        const std::string& output_path)
    {
        service_configs_.emplace_back(type, output_path);
    }

    void multiplexing_telemetry_service::start_test(
        const std::string& test_suite_name,
        const std::string& name)
    {
        // Clear existing children
        children_.clear();

        // Recreate services based on cached configurations with current test info
        for (const auto& config : service_configs_)
        {
            std::shared_ptr<i_telemetry_service> service;

            if (create_test_telemetry_service(service, config.type, test_suite_name, name, config.output_path))
            {
                children_.push_back(service);
            }
        }
    }

    void multiplexing_telemetry_service::reset_for_test()
    {
        // Clear existing children
        children_.clear();
    }
#endif

    // Forward all telemetry events to children
    void multiplexing_telemetry_service::handle_telemetry_event(rpc::telemetry_event event) const
    {
#define RPC_TELEMETRY_DECODE_AND_FORWARD(event_type, handler_name)                                                     \
    if (event.event_type_id == rpc::id<rpc::telemetry::event_type>::get(rpc::get_version()))                           \
    {                                                                                                                  \
        rpc::telemetry::event_type decoded_event;                                                                      \
        if (rpc::from_yas_binary(rpc::byte_span(event.payload), decoded_event).empty())                                \
            handler_name(decoded_event);                                                                               \
        return;                                                                                                        \
    }

        RPC_TELEMETRY_DECODE_AND_FORWARD(service_creation_event, on_service_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_deletion_event, on_service_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_send_event, on_service_send)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_post_event, on_service_post)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_try_cast_event, on_service_try_cast)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_add_ref_event, on_service_add_ref)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_release_event, on_service_release)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_object_released_event, on_service_object_released)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_transport_down_event, on_service_transport_down)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_creation_event, on_service_proxy_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(cloned_service_proxy_creation_event, on_cloned_service_proxy_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_deletion_event, on_service_proxy_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_send_event, on_service_proxy_send)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_post_event, on_service_proxy_post)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_try_cast_event, on_service_proxy_try_cast)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_add_ref_event, on_service_proxy_add_ref)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_release_event, on_service_proxy_release)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_object_released_event, on_service_proxy_object_released)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_transport_down_event, on_service_proxy_transport_down)
        RPC_TELEMETRY_DECODE_AND_FORWARD(service_proxy_external_ref_event, on_service_proxy_add_external_ref)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_creation_event, on_transport_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_deletion_event, on_transport_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_status_change_event, on_transport_status_change)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_destination_event, on_transport_add_destination)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_accept_event, on_transport_accept)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_send_event, on_transport_outbound_send)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_post_event, on_transport_outbound_post)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_try_cast_event, on_transport_outbound_try_cast)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_add_ref_event, on_transport_outbound_add_ref)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_release_event, on_transport_outbound_release)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_object_released_event, on_transport_outbound_object_released)
        RPC_TELEMETRY_DECODE_AND_FORWARD(transport_transport_down_event, on_transport_outbound_transport_down)
        RPC_TELEMETRY_DECODE_AND_FORWARD(impl_creation_event, on_impl_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(impl_deletion_event, on_impl_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(stub_creation_event, on_stub_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(stub_deletion_event, on_stub_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(stub_send_event, on_stub_send)
        RPC_TELEMETRY_DECODE_AND_FORWARD(stub_add_ref_event, on_stub_add_ref)
        RPC_TELEMETRY_DECODE_AND_FORWARD(stub_release_event, on_stub_release)
        RPC_TELEMETRY_DECODE_AND_FORWARD(object_proxy_creation_event, on_object_proxy_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(object_proxy_deletion_event, on_object_proxy_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(interface_proxy_creation_event, on_interface_proxy_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(interface_proxy_deletion_event, on_interface_proxy_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(interface_proxy_send_event, on_interface_proxy_send)
        RPC_TELEMETRY_DECODE_AND_FORWARD(pass_through_creation_event, on_pass_through_creation)
        RPC_TELEMETRY_DECODE_AND_FORWARD(pass_through_deletion_event, on_pass_through_deletion)
        RPC_TELEMETRY_DECODE_AND_FORWARD(pass_through_add_ref_event, on_pass_through_add_ref)
        RPC_TELEMETRY_DECODE_AND_FORWARD(pass_through_release_event, on_pass_through_release)
        RPC_TELEMETRY_DECODE_AND_FORWARD(pass_through_status_change_event, on_pass_through_status_change)

#undef RPC_TELEMETRY_DECODE_AND_FORWARD

        if (event.event_type_id == rpc::id<rpc::log_record>::get(rpc::get_version()))
        {
            rpc::log_record decoded_event;
            if (rpc::from_yas_binary(rpc::byte_span(event.payload), decoded_event).empty())
                message(decoded_event);
            return;
        }

        return;
    }

    void multiplexing_telemetry_service::on_service_creation(const rpc::telemetry::service_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto parent_zone_id = event.parent_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_creation({name, zone_id, parent_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_deletion(const rpc::telemetry::service_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_deletion({zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_try_cast({zone_id, remote_object_id, caller_zone_id, interface_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_add_ref({zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_release(const rpc::telemetry::service_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_release({zone_id, remote_object_id, caller_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_send(const rpc::telemetry::service_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_send({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_post(const rpc::telemetry::service_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_post({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_object_released(
        const rpc::telemetry::service_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_object_released({zone_id, remote_object_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_transport_down(
        const rpc::telemetry::service_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_transport_down({zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_creation(
        const rpc::telemetry::service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_creation(
                    {service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_cloned_service_proxy_creation(
        const rpc::telemetry::cloned_service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_cloned_service_proxy_creation(
                    {service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_deletion(
        const rpc::telemetry::service_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_deletion({zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_try_cast(
        const rpc::telemetry::service_proxy_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_try_cast({zone_id, remote_object_id, caller_zone_id, interface_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_add_ref(
        const rpc::telemetry::service_proxy_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_add_ref({zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_release(
        const rpc::telemetry::service_proxy_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_release({zone_id, remote_object_id, caller_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_add_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_add_external_ref({zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_release_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_release_external_ref({zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_impl_creation(const rpc::telemetry::impl_creation_event& event) const
    {
        const auto& name = event.name;
        auto address = event.address;
        auto zone_id = event.zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_impl_creation({name, address, zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const
    {
        auto address = event.address;
        auto zone_id = event.zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_impl_deletion({address, zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_stub_creation(const rpc::telemetry::stub_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto address = event.address;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_stub_creation({zone_id, object_id, address});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_stub_deletion({zone_id, object_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_stub_send(const rpc::telemetry::stub_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_stub_send({zone_id, object_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_stub_add_ref({destination_zone_id, object_id, interface_id, count, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_stub_release(const rpc::telemetry::stub_release_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_stub_release({destination_zone_id, object_id, interface_id, count, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_object_proxy_creation(
        const rpc::telemetry::object_proxy_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto add_ref_done = event.add_ref_done;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_object_proxy_creation({zone_id, destination_zone_id, object_id, add_ref_done});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_object_proxy_deletion(
        const rpc::telemetry::object_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_object_proxy_deletion({zone_id, destination_zone_id, object_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_interface_proxy_creation(
        const rpc::telemetry::interface_proxy_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_interface_proxy_creation({name, zone_id, destination_zone_id, object_id, interface_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_interface_proxy_deletion(
        const rpc::telemetry::interface_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_interface_proxy_deletion({zone_id, destination_zone_id, object_id, interface_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const
    {
        const auto& method_name = event.method_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_interface_proxy_send(
                    {method_name, zone_id, destination_zone_id, object_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::message(const rpc::log_record& event) const
    {
        auto level = static_cast<level_enum>(event.level);
        const auto& message = event.message;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->message({level, message});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_creation(const rpc::telemetry::transport_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto status = event.status;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_creation({name, zone_id, adjacent_zone_id, status});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_deletion({zone_id, adjacent_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_status_change(
        const rpc::telemetry::transport_status_change_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto old_status = event.old_status;
        auto new_status = event.new_status;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_status_change({name, zone_id, adjacent_zone_id, old_status, new_status});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_add_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_add_destination({zone_id, adjacent_zone_id, destination, caller});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_remove_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_remove_destination({zone_id, adjacent_zone_id, destination, caller});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_accept(const rpc::telemetry::transport_accept_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto result = event.result;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_accept({zone_id, adjacent_zone_id, result});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_pass_through_creation(
        const rpc::telemetry::pass_through_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_count = event.shared_count;
        auto optimistic_count = event.optimistic_count;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_pass_through_creation(
                    {zone_id, forward_destination, reverse_destination, shared_count, optimistic_count});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_pass_through_deletion(
        const rpc::telemetry::pass_through_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_pass_through_deletion({zone_id, forward_destination, reverse_destination});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto options = event.options;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_pass_through_add_ref(
                    {zone_id, forward_destination, reverse_destination, options, shared_delta, optimistic_delta});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_pass_through_release(
                    {zone_id, forward_destination, reverse_destination, shared_delta, optimistic_delta});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_pass_through_status_change(
        const rpc::telemetry::pass_through_status_change_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto forward_status = event.forward_status;
        auto reverse_status = event.reverse_status;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_pass_through_status_change(
                    {zone_id, forward_destination, reverse_destination, forward_status, reverse_status});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_send({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_post({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_object_released(
        const rpc::telemetry::service_proxy_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_object_released({zone_id, remote_object_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_service_proxy_transport_down(
        const rpc::telemetry::service_proxy_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_service_proxy_transport_down({zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_send(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_post(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_try_cast(
        const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_try_cast(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_add_ref(
        const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_add_ref(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_release(
        const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_release(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_object_released({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_outbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_outbound_transport_down(
                    {zone_id, adjacent_zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_send(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_post(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_try_cast(
        const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_try_cast(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_add_ref(
        const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_add_ref(
                    {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_release(
        const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_release({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, options});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_object_released({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id});
            }
        }
        return;
    }

    void multiplexing_telemetry_service::on_transport_inbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        for (const auto& child : children_)
        {
            if (child)
            {
                child->on_transport_inbound_transport_down(
                    {zone_id, adjacent_zone_id, destination_zone_id, caller_zone_id});
            }
        }
        return;
    }
}
