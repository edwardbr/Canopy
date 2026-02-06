/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/telemetry/animation_telemetry_service.h>

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

namespace rpc
{
    namespace
    {
        constexpr const char* kTitlePrefix = "RPC++ Telemetry Animation";
    }

    bool animation_telemetry_service::create(std::shared_ptr<rpc::i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        auto fixed_suite = sanitize_name(test_suite_name);
        std::filesystem::path output_directory = directory / fixed_suite;
        std::error_code ec;
        std::filesystem::create_directories(output_directory, ec);

        auto output_path = output_directory / (name + ".html");

        service = std::shared_ptr<rpc::i_telemetry_service>(
            new animation_telemetry_service(output_path, test_suite_name, name));
        return true;
    }

    animation_telemetry_service::animation_telemetry_service(
        std::filesystem::path output_path, std::string test_suite_name, std::string test_name)
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
        const std::string& key, const std::string& value)
    {
        return event_field{key, value, field_kind::string};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_number_field(
        const std::string& key, uint64_t value)
    {
        return event_field{key, std::to_string(value), field_kind::number};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_signed_field(
        const std::string& key, int64_t value)
    {
        return event_field{key, std::to_string(value), field_kind::number};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_boolean_field(
        const std::string& key, bool value)
    {
        return event_field{key, value ? "true" : "false", field_kind::boolean};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_floating_field(
        const std::string& key, double value)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return event_field{key, oss.str(), field_kind::floating};
    }

    void animation_telemetry_service::record_event(const std::string& type, std::initializer_list<event_field> fields) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_record record;
        record.timestamp = timestamp_now();
        record.type = type;
        record.fields.assign(fields.begin(), fields.end());
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::record_event(const std::string& type, std::vector<event_field>&& fields) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_record record;
        record.timestamp = timestamp_now();
        record.type = type;
        record.fields = std::move(fields);
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::on_service_creation(
        const std::string& name, rpc::zone zone_id, rpc::destination_zone parent_zone_id) const
    {
        double ts = timestamp_now();
        std::vector<event_field> fields;
        fields.reserve(3);
        fields.push_back(make_string_field("serviceName", name));
        fields.push_back(make_number_field("zone", zone_id.get_val()));
        fields.push_back(make_number_field("parentZone", parent_zone_id.get_val()));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            zone_names_[zone_id.get_val()] = name;
            zone_parents_[zone_id.get_val()] = parent_zone_id.get_val();

            event_record record;
            record.timestamp = ts;
            record.type = "service_creation";
            record.fields = std::move(fields);
            events_.push_back(std::move(record));
        }
    }

    void animation_telemetry_service::on_service_deletion(rpc::zone zone_id) const
    {
        std::vector<event_field> fields = {make_number_field("zone", zone_id.get_val())};
        std::lock_guard<std::mutex> lock(mutex_);
        zone_names_.erase(zone_id.get_val());
        zone_parents_.erase(zone_id.get_val());

        event_record record;
        record.timestamp = timestamp_now();
        record.type = "service_deletion";
        record.fields = std::move(fields);
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::on_service_try_cast(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("service_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_service_add_ref(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        record_event("service_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_service_release(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options) const
    {
        record_event("service_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_service_proxy_creation(const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::vector<event_field> fields = {make_string_field("serviceName", service_name),
            make_string_field("serviceProxyName", service_proxy_name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("callerZone", caller_zone_id.get_val())};
        record_event("service_proxy_creation", std::move(fields));
    }

    void animation_telemetry_service::on_cloned_service_proxy_creation(const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::vector<event_field> fields = {make_string_field("serviceName", service_name),
            make_string_field("serviceProxyName", service_proxy_name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("callerZone", caller_zone_id.get_val())};
        record_event("cloned_service_proxy_creation", std::move(fields));
    }

    void animation_telemetry_service::on_service_proxy_deletion(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_proxy_deletion",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_try_cast(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("service_proxy_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_add_ref(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        record_event("service_proxy_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_service_proxy_release(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options) const
    {
        record_event("service_proxy_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_service_proxy_add_external_ref(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_proxy_add_external_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_release_external_ref(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_proxy_release_external_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_impl_creation(const std::string& name, uint64_t address, rpc::zone zone_id) const
    {
        std::vector<event_field> fields = {make_string_field("name", name),
            make_number_field("address", address),
            make_number_field("zone", zone_id.get_val())};
        record_event("impl_creation", std::move(fields));
    }

    void animation_telemetry_service::on_impl_deletion(uint64_t address, rpc::zone zone_id) const
    {
        record_event(
            "impl_deletion", {make_number_field("address", address), make_number_field("zone", zone_id.get_val())});
    }

    void animation_telemetry_service::on_stub_creation(rpc::zone zone_id, rpc::object object_id, uint64_t address) const
    {
        record_event("stub_creation",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("address", address)});
    }

    void animation_telemetry_service::on_stub_deletion(rpc::zone zone_id, rpc::object object_id) const
    {
        record_event("stub_deletion",
            {make_number_field("zone", zone_id.get_val()), make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_stub_send(
        rpc::zone zone_id, rpc::object object_id, rpc::interface_ordinal interface_id, rpc::method method_id) const
    {
        record_event("stub_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_stub_add_ref(rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("stub_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("count", count),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_stub_release(rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("stub_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("count", count),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_object_proxy_creation(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id, bool add_ref_done) const
    {
        record_event("object_proxy_creation",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_boolean_field("addRefDone", add_ref_done)});
    }

    void animation_telemetry_service::on_object_proxy_deletion(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id) const
    {
        record_event("object_proxy_deletion",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_interface_proxy_creation(const std::string& name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        std::vector<event_field> fields = {make_string_field("name", name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("object", object_id.get_val()),
            make_number_field("interface", interface_id.get_val())};
        record_event("interface_proxy_creation", std::move(fields));
    }

    void animation_telemetry_service::on_interface_proxy_deletion(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("interface_proxy_deletion",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_interface_proxy_send(const std::string& method_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::vector<event_field> fields = {make_string_field("methodName", method_name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("object", object_id.get_val()),
            make_number_field("interface", interface_id.get_val()),
            make_number_field("method", method_id.get_val())};
        record_event("interface_proxy_send", std::move(fields));
    }

    void animation_telemetry_service::message(level_enum level, const std::string& message) const
    {
        std::vector<event_field> fields
            = {make_number_field("level", static_cast<uint64_t>(level)), make_string_field("message", message)};
        record_event("message", std::move(fields));
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

    void animation_telemetry_service::on_transport_creation(
        const std::string& name, rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::transport_status status) const
    {
        record_event("transport_creation",
            {make_string_field("name", name),
                make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("status", static_cast<uint32_t>(status))});
    }

    void animation_telemetry_service::on_transport_deletion(rpc::zone zone_id, rpc::zone adjacent_zone_id) const
    {
        record_event("transport_deletion",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val())});
    }

    void animation_telemetry_service::on_transport_status_change(const std::string& name,
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::transport_status old_status,
        rpc::transport_status new_status) const
    {
        record_event("transport_status_change",
            {make_string_field("name", name),
                make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("old_status", static_cast<uint32_t>(old_status)),
                make_number_field("new_status", static_cast<uint32_t>(new_status))});
    }

    void animation_telemetry_service::on_transport_add_destination(
        rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::destination_zone destination, rpc::caller_zone caller) const
    {
        record_event("transport_add_destination",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("destination", destination.get_val()),
                make_number_field("caller", caller.get_val())});
    }

    void animation_telemetry_service::on_transport_remove_destination(
        rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::destination_zone destination, rpc::caller_zone caller) const
    {
        record_event("transport_remove_destination",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("destination", destination.get_val()),
                make_number_field("caller", caller.get_val())});
    }

    void animation_telemetry_service::on_transport_accept(rpc::zone zone_id, rpc::zone adjacent_zone_id, int result) const
    {
        record_event("transport_accept",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_signed_field("result", result)});
    }

    void animation_telemetry_service::on_pass_through_creation(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        uint64_t shared_count,
        uint64_t optimistic_count) const
    {
        record_event("pass_through_creation",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_number_field("shared_count", shared_count),
                make_number_field("optimistic_count", optimistic_count)});
    }

    void animation_telemetry_service::on_pass_through_deletion(
        rpc::zone zone_id, rpc::destination_zone forward_destination, rpc::destination_zone reverse_destination) const
    {
        record_event("pass_through_deletion",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val())});
    }

    void animation_telemetry_service::on_pass_through_add_ref(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::add_ref_options options,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        record_event("pass_through_add_ref",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_signed_field("shared_delta", shared_delta),
                make_signed_field("optimistic_delta", optimistic_delta)});
    }

    void animation_telemetry_service::on_pass_through_release(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        record_event("pass_through_release",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_signed_field("shared_delta", shared_delta),
                make_signed_field("optimistic_delta", optimistic_delta)});
    }

    void animation_telemetry_service::on_pass_through_status_change(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::transport_status forward_status,
        rpc::transport_status reverse_status) const
    {
        record_event("pass_through_status_change",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_number_field("forward_status", static_cast<uint32_t>(forward_status)),
                make_number_field("reverse_status", static_cast<uint32_t>(reverse_status))});
    }

    // Service methods
    void animation_telemetry_service::on_service_send(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_post(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_object_released(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("service_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_service_transport_down(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    // Service proxy methods
    void animation_telemetry_service::on_service_proxy_send(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_proxy_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_post(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_proxy_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_object_released(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("service_proxy_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_transport_down(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_proxy_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    // Transport outbound methods
    void animation_telemetry_service::on_transport_outbound_send(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_outbound_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_post(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_outbound_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_try_cast(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("transport_outbound_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_add_ref(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        record_event("transport_outbound_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_transport_outbound_release(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options) const
    {
        record_event("transport_outbound_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_transport_outbound_object_released(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("transport_outbound_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_transport_down(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("transport_outbound_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    // Transport inbound methods
    void animation_telemetry_service::on_transport_inbound_send(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_inbound_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_post(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_inbound_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_try_cast(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("transport_inbound_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_add_ref(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        record_event("transport_inbound_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_transport_inbound_release(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options) const
    {
        record_event("transport_inbound_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options))});
    }

    void animation_telemetry_service::on_transport_inbound_object_released(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("transport_inbound_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_transport_down(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("transport_inbound_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }
}
