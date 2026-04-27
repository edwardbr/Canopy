/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/telemetry/telemetry_service_factory.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace
{
    std::filesystem::path animation_template_root()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path() / "html_templates" / "animation";
    }

    std::string template_base_url(const std::filesystem::path& output_dir)
    {
        std::error_code ec;
        std::filesystem::path root = animation_template_root();
        std::filesystem::path relative = std::filesystem::relative(root, output_dir, ec);
        std::filesystem::path chosen = (!ec && !relative.empty()) ? relative : root;
        std::string base = chosen.generic_string();
        if (!base.empty() && base.back() != '/')
        {
            base.push_back('/');
        }
        if (chosen.is_absolute())
        {
            return "file://" + base;
        }
        return base;
    }
}

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rpc/telemetry/i_telemetry_service.h>

namespace rpc::telemetry
{
    class animation_telemetry_service : public i_telemetry_service
    {
    public:
        static bool create(
            std::shared_ptr<i_telemetry_service>& service,
            const std::string& test_suite_name,
            const std::string& name,
            const std::filesystem::path& directory);

        ~animation_telemetry_service() override;

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

    private:
        enum class field_kind
        {
            string,
            number,
            boolean,
            floating
        };

        struct event_field
        {
            std::string key;
            std::string value;
            field_kind type;
        };

        struct event_record
        {
            double timestamp = 0.0;
            std::string type;
            std::vector<event_field> fields;
        };

        animation_telemetry_service(
            std::filesystem::path output_path,
            std::string test_suite_name,
            std::string test_name);

        static std::string sanitize_name(const std::string& name);
        static std::string escape_json(const std::string& input);
        static event_field make_string_field(
            const std::string& key,
            const std::string& value);
        static event_field make_number_field(
            const std::string& key,
            uint64_t value);
        static event_field make_signed_field(
            const std::string& key,
            int64_t value);
        static event_field make_boolean_field(
            const std::string& key,
            bool value);
        static event_field make_floating_field(
            const std::string& key,
            double value);

        void record_event(
            const std::string& type,
            std::initializer_list<event_field> fields = {}) const;
        void record_event(
            const std::string& type,
            std::vector<event_field>&& fields) const;
        void write_output() const;

        inline double timestamp_now() const
        {
            auto now = std::chrono::steady_clock::now();
            auto delta = std::chrono::duration<double>(now - start_time_);
            return delta.count();
        }

        mutable std::mutex mutex_;
        mutable std::vector<event_record> events_;
        mutable std::unordered_map<uint64_t, std::string> zone_names_;
        mutable std::unordered_map<uint64_t, uint64_t> zone_parents_;

        std::filesystem::path output_path_;
        std::string suite_name_;
        std::string test_name_;
        std::chrono::steady_clock::time_point start_time_;
    };
}

namespace rpc::telemetry
{
    namespace
    {
        constexpr const char* kTitlePrefix = "RPC++ Telemetry Animation";
    }

    bool animation_telemetry_service::create(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        auto fixed_suite = sanitize_name(test_suite_name);
        std::filesystem::path output_directory = directory / fixed_suite;
        std::error_code ec;
        std::filesystem::create_directories(output_directory, ec);

        auto output_path = output_directory / (name + ".html");

        service
            = std::shared_ptr<i_telemetry_service>(new animation_telemetry_service(output_path, test_suite_name, name));
        return true;
    }

    bool create_animation_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        return animation_telemetry_service::create(service, test_suite_name, name, directory);
    }

    animation_telemetry_service::animation_telemetry_service(
        std::filesystem::path output_path,
        std::string test_suite_name,
        std::string test_name)
        : output_path_(std::move(output_path))
        , suite_name_(std::move(test_suite_name))
        , test_name_(std::move(test_name))
        , start_time_(std::chrono::steady_clock::now())
    {
    }

    animation_telemetry_service::~animation_telemetry_service()
    {
        write_output();
    }

    std::string animation_telemetry_service::sanitize_name(const std::string& name)
    {
        std::string sanitized = name;
        std::replace_if(
            sanitized.begin(), sanitized.end(), [](char ch) { return ch == '/' || ch == '\\' || ch == ':' || ch == '*'; }, '#');
        return sanitized;
    }

    std::string animation_telemetry_service::escape_json(const std::string& input)
    {
        std::string escaped;
        escaped.reserve(input.size() + input.size() / 4 + 4);
        for (char ch : input)
        {
            switch (ch)
            {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch));
                    escaped += oss.str();
                }
                else
                {
                    escaped += ch;
                }
            }
        }
        return escaped;
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_string_field(
        const std::string& key,
        const std::string& value)
    {
        return event_field{key, value, field_kind::string};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_number_field(
        const std::string& key,
        uint64_t value)
    {
        return event_field{key, std::to_string(value), field_kind::number};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_signed_field(
        const std::string& key,
        int64_t value)
    {
        return event_field{key, std::to_string(value), field_kind::number};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_boolean_field(
        const std::string& key,
        bool value)
    {
        return event_field{key, value ? "true" : "false", field_kind::boolean};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_floating_field(
        const std::string& key,
        double value)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return event_field{key, oss.str(), field_kind::floating};
    }

    void animation_telemetry_service::record_event(
        const std::string& type,
        std::initializer_list<event_field> fields) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_record record;
        record.timestamp = timestamp_now();
        record.type = type;
        record.fields.assign(fields.begin(), fields.end());
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::record_event(
        const std::string& type,
        std::vector<event_field>&& fields) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_record record;
        record.timestamp = timestamp_now();
        record.type = type;
        record.fields = std::move(fields);
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::on_service_creation(const rpc::telemetry::service_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto parent_zone_id = event.parent_zone_id;
        double ts = timestamp_now();
        std::vector<event_field> fields;
        fields.reserve(3);
        fields.push_back(make_string_field("serviceName", name));
        fields.push_back(make_number_field("zone", zone_id.get_subnet()));
        fields.push_back(make_number_field("parentZone", parent_zone_id.get_subnet()));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            zone_names_[zone_id.get_subnet()] = name;
            zone_parents_[zone_id.get_subnet()] = parent_zone_id.get_subnet();

            event_record record;
            record.timestamp = ts;
            record.type = "service_creation";
            record.fields = std::move(fields);
            events_.push_back(std::move(record));
        }
        return;
    }

    void animation_telemetry_service::on_service_deletion(const rpc::telemetry::service_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        std::vector<event_field> fields = {make_number_field("zone", zone_id.get_subnet())};
        std::lock_guard<std::mutex> lock(mutex_);
        zone_names_.erase(zone_id.get_subnet());
        zone_parents_.erase(zone_id.get_subnet());

        event_record record;
        record.timestamp = timestamp_now();
        record.type = "service_deletion";
        record.fields = std::move(fields);
        events_.push_back(std::move(record));
        return;
    }

    void animation_telemetry_service::on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_try_cast",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_add_ref",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("knownDirectionZone", requesting_zone_id.get_subnet()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_service_release(const rpc::telemetry::service_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_release",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_service_proxy_creation(const rpc::telemetry::service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::vector<event_field> fields = {make_string_field("serviceName", service_name),
            make_string_field("serviceProxyName", service_proxy_name),
            make_number_field("zone", zone_id.get_subnet()),
            make_number_field("destinationZone", destination_zone_id.get_subnet()),
            make_number_field("callerZone", caller_zone_id.get_subnet())};
        record_event("service_proxy_creation", std::move(fields));
        return;
    }

    void animation_telemetry_service::on_cloned_service_proxy_creation(
        const rpc::telemetry::cloned_service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::vector<event_field> fields = {make_string_field("serviceName", service_name),
            make_string_field("serviceProxyName", service_proxy_name),
            make_number_field("zone", zone_id.get_subnet()),
            make_number_field("destinationZone", destination_zone_id.get_subnet()),
            make_number_field("callerZone", caller_zone_id.get_subnet())};
        record_event("cloned_service_proxy_creation", std::move(fields));
        return;
    }

    void animation_telemetry_service::on_service_proxy_deletion(const rpc::telemetry::service_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "service_proxy_deletion",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_service_proxy_try_cast(const rpc::telemetry::service_proxy_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_proxy_try_cast",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_proxy_add_ref(const rpc::telemetry::service_proxy_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_proxy_add_ref",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", requesting_zone_id.get_subnet()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_service_proxy_release(const rpc::telemetry::service_proxy_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_proxy_release",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_service_proxy_add_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "service_proxy_add_external_ref",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_service_proxy_release_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "service_proxy_release_external_ref",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_impl_creation(const rpc::telemetry::impl_creation_event& event) const
    {
        const auto& name = event.name;
        auto address = event.address;
        auto zone_id = event.zone_id;
        std::vector<event_field> fields = {make_string_field("name", name),
            make_number_field("address", address),
            make_number_field("zone", zone_id.get_subnet())};
        record_event("impl_creation", std::move(fields));
        return;
    }

    void animation_telemetry_service::on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const
    {
        auto address = event.address;
        auto zone_id = event.zone_id;
        record_event(
            "impl_deletion", {make_number_field("address", address), make_number_field("zone", zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_stub_creation(const rpc::telemetry::stub_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto address = event.address;
        record_event(
            "stub_creation",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("address", address)});
        return;
    }

    void animation_telemetry_service::on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        record_event(
            "stub_deletion",
            {make_number_field("zone", zone_id.get_subnet()), make_number_field("object", object_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_stub_send(const rpc::telemetry::stub_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        record_event(
            "stub_send",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "stub_add_ref",
            {make_number_field("zone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("count", count),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_stub_release(const rpc::telemetry::stub_release_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "stub_release",
            {make_number_field("zone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("count", count),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_object_proxy_creation(const rpc::telemetry::object_proxy_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto add_ref_done = event.add_ref_done;
        record_event(
            "object_proxy_creation",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_boolean_field("addRefDone", add_ref_done)});
        return;
    }

    void animation_telemetry_service::on_object_proxy_deletion(const rpc::telemetry::object_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        record_event(
            "object_proxy_deletion",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_interface_proxy_creation(
        const rpc::telemetry::interface_proxy_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        std::vector<event_field> fields = {make_string_field("name", name),
            make_number_field("zone", zone_id.get_subnet()),
            make_number_field("destinationZone", destination_zone_id.get_subnet()),
            make_number_field("object", object_id.get_val()),
            make_number_field("interface", interface_id.get_val())};
        record_event("interface_proxy_creation", std::move(fields));
        return;
    }

    void animation_telemetry_service::on_interface_proxy_deletion(
        const rpc::telemetry::interface_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        record_event(
            "interface_proxy_deletion",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const
    {
        const auto& method_name = event.method_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::vector<event_field> fields = {make_string_field("methodName", method_name),
            make_number_field("zone", zone_id.get_subnet()),
            make_number_field("destinationZone", destination_zone_id.get_subnet()),
            make_number_field("object", object_id.get_val()),
            make_number_field("interface", interface_id.get_val()),
            make_number_field("method", method_id.get_val())};
        record_event("interface_proxy_send", std::move(fields));
        return;
    }

    void animation_telemetry_service::message(const rpc::log_record& event) const
    {
        auto level = static_cast<level_enum>(event.level);
        const auto& message = event.message;
        std::vector<event_field> fields
            = {make_number_field("level", static_cast<uint64_t>(level)), make_string_field("message", message)};
        record_event("message", std::move(fields));
        return;
    }

    void animation_telemetry_service::write_output() const
    {
        std::vector<event_record> events_copy;
        std::unordered_map<uint64_t, std::string> zone_names_copy;
        std::unordered_map<uint64_t, uint64_t> zone_parents_copy;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_copy = events_;
            zone_names_copy = zone_names_;
            zone_parents_copy = zone_parents_;
        }

        std::error_code ec;
        std::filesystem::create_directories(output_path_.parent_path(), ec);

        std::ofstream output(output_path_, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output)
        {
            return;
        }

        double total_duration = 0.0;
        if (!events_copy.empty())
        {
            total_duration = events_copy.back().timestamp;
            for (const auto& evt : events_copy)
            {
                if (evt.timestamp > total_duration)
                {
                    total_duration = evt.timestamp;
                }
            }
        }

        const std::string template_base = template_base_url(output_path_.parent_path());

        output << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\" />\n";
        output << "<title>" << kTitlePrefix << " - " << escape_json(suite_name_) << "." << escape_json(test_name_)
               << "</title>\n";
        output << "<style>\n";
        output << "html, body { margin: 0; height: 100%; }\n";
        output << "#telemetry-frame { border: 0; width: 100%; height: 100%; }\n";
        output << "</style>\n";
        output << "</head>\n<body>\n";
        output << "<iframe id=\"telemetry-frame\" src=\"" << escape_json(template_base) << "layout.html\"></iframe>\n";

        output << "<script>\n";
        output << "const telemetryMeta = { suite: \"" << escape_json(suite_name_) << "\", test: \""
               << escape_json(test_name_) << "\" };\n";

        output << "const events = [\n";
        for (size_t idx = 0; idx < events_copy.size(); ++idx)
        {
            const auto& evt = events_copy[idx];
            output << "  { type: \"" << escape_json(evt.type) << "\", timestamp: " << std::fixed << std::setprecision(6)
                   << evt.timestamp << ", data: {";
            for (size_t field_idx = 0; field_idx < evt.fields.size(); ++field_idx)
            {
                const auto& field = evt.fields[field_idx];
                if (field_idx > 0)
                {
                    output << ", ";
                }
                output << "\"" << field.key << "\": ";
                switch (field.type)
                {
                case field_kind::string:
                    output << "\"" << escape_json(field.value) << "\"";
                    break;
                case field_kind::number:
                    output << field.value;
                    break;
                case field_kind::boolean:
                    output << (field.value == "true" ? "true" : "false");
                    break;
                case field_kind::floating:
                    output << field.value;
                    break;
                }
            }
            output << " } }";
            if (idx + 1 < events_copy.size())
            {
                output << ",";
            }
            output << "\n";
        }
        output << "];\n";
        output << "const totalDuration = " << std::fixed << std::setprecision(6) << total_duration << ";\n";
        output << "const payload = { telemetryMeta, events, totalDuration };\n";
        output << "const frame = document.getElementById('telemetry-frame');\n";
        output << "function postPayload() {\n";
        output << "  if (!frame || !frame.contentWindow) {\n";
        output << "    return;\n";
        output << "  }\n";
        output << "  frame.contentWindow.postMessage(payload, '*');\n";
        output << "}\n";
        output << "frame.addEventListener('load', () => {\n";
        output << "  postPayload();\n";
        output << "  setTimeout(postPayload, 250);\n";
        output << "  setTimeout(postPayload, 1000);\n";
        output << "});\n";
        output << "</script>\n";
        output << "</body>\n</html>\n";
    }

    void animation_telemetry_service::on_transport_creation(const rpc::telemetry::transport_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto status = event.status;
        record_event(
            "transport_creation",
            {make_string_field("name", name),
                make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_subnet()),
                make_number_field("status", static_cast<uint32_t>(status))});
        return;
    }

    void animation_telemetry_service::on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        record_event(
            "transport_deletion",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_transport_status_change(
        const rpc::telemetry::transport_status_change_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto old_status = event.old_status;
        auto new_status = event.new_status;
        record_event(
            "transport_status_change",
            {make_string_field("name", name),
                make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_subnet()),
                make_number_field("old_status", static_cast<uint32_t>(old_status)),
                make_number_field("new_status", static_cast<uint32_t>(new_status))});
        return;
    }

    void animation_telemetry_service::on_transport_add_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        record_event(
            "transport_add_destination",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_subnet()),
                make_number_field("destination", destination.get_subnet()),
                make_number_field("caller", caller.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_transport_remove_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        record_event(
            "transport_remove_destination",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_subnet()),
                make_number_field("destination", destination.get_subnet()),
                make_number_field("caller", caller.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_transport_accept(const rpc::telemetry::transport_accept_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto result = event.result;
        record_event(
            "transport_accept",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_subnet()),
                make_signed_field("result", result)});
        return;
    }

    void animation_telemetry_service::on_pass_through_creation(const rpc::telemetry::pass_through_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_count = event.shared_count;
        auto optimistic_count = event.optimistic_count;
        record_event(
            "pass_through_creation",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("forward_destination", forward_destination.get_subnet()),
                make_number_field("reverse_destination", reverse_destination.get_subnet()),
                make_number_field("shared_count", shared_count),
                make_number_field("optimistic_count", optimistic_count)});
        return;
    }

    void animation_telemetry_service::on_pass_through_deletion(const rpc::telemetry::pass_through_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        record_event(
            "pass_through_deletion",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("forward_destination", forward_destination.get_subnet()),
                make_number_field("reverse_destination", reverse_destination.get_subnet())});
        return;
    }

    void animation_telemetry_service::on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto options = event.options;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        record_event(
            "pass_through_add_ref",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("forward_destination", forward_destination.get_subnet()),
                make_number_field("reverse_destination", reverse_destination.get_subnet()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_signed_field("shared_delta", shared_delta),
                make_signed_field("optimistic_delta", optimistic_delta)});
        return;
    }

    void animation_telemetry_service::on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        record_event(
            "pass_through_release",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("forward_destination", forward_destination.get_subnet()),
                make_number_field("reverse_destination", reverse_destination.get_subnet()),
                make_signed_field("shared_delta", shared_delta),
                make_signed_field("optimistic_delta", optimistic_delta)});
        return;
    }

    void animation_telemetry_service::on_pass_through_status_change(
        const rpc::telemetry::pass_through_status_change_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto forward_status = event.forward_status;
        auto reverse_status = event.reverse_status;
        record_event(
            "pass_through_status_change",
            {make_number_field("zone_id", zone_id.get_subnet()),
                make_number_field("forward_destination", forward_destination.get_subnet()),
                make_number_field("reverse_destination", reverse_destination.get_subnet()),
                make_number_field("forward_status", static_cast<uint32_t>(forward_status)),
                make_number_field("reverse_status", static_cast<uint32_t>(reverse_status))});
        return;
    }

    // Service methods
    void animation_telemetry_service::on_service_send(const rpc::telemetry::service_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_send",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_post(const rpc::telemetry::service_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_post",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_object_released(
        const rpc::telemetry::service_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_object_released",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_transport_down(const rpc::telemetry::service_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "service_transport_down",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    // Service proxy methods
    void animation_telemetry_service::on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_proxy_send",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_proxy_post",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_proxy_object_released(
        const rpc::telemetry::service_proxy_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "service_proxy_object_released",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_service_proxy_transport_down(
        const rpc::telemetry::service_proxy_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "service_proxy_transport_down",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    // Transport outbound methods
    void animation_telemetry_service::on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_outbound_send",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_outbound_post",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_outbound_try_cast(
        const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_outbound_try_cast",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_outbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_outbound_add_ref",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", requesting_zone_id.get_subnet()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_transport_outbound_release(const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_outbound_release",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_transport_outbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_outbound_object_released",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_outbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "transport_outbound_transport_down",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }

    // Transport inbound methods
    void animation_telemetry_service::on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_inbound_send",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_inbound_post",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_inbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_inbound_try_cast",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_inbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_inbound_add_ref",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", requesting_zone_id.get_subnet()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_transport_inbound_release(const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_inbound_release",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
        return;
    }

    void animation_telemetry_service::on_transport_inbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        record_event(
            "transport_inbound_object_released",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet()),
                make_number_field("object", object_id.get_val())});
        return;
    }

    void animation_telemetry_service::on_transport_inbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        record_event(
            "transport_inbound_transport_down",
            {make_number_field("zone", zone_id.get_subnet()),
                make_number_field("adjacentZone", adjacent_zone_id.get_subnet()),
                make_number_field("destinationZone", destination_zone_id.get_subnet()),
                make_number_field("callerZone", caller_zone_id.get_subnet())});
        return;
    }
}
