/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
// Standard C++ headers
#include <chrono>
#include <thread>

// RPC headers
#include <rpc/rpc.h>
#include <rpc/telemetry/telemetry_service_factory.h>

// Other headers
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <filesystem>
#include <unordered_map>
#include <set>
#include <memory>
#include <rpc/rpc.h>
// types.h is included via i_telemetry_service.h
#include <rpc/telemetry/i_telemetry_service.h>

namespace spdlog
{
    class logger;
}

namespace rpc::telemetry
{
    class console_telemetry_service : public i_telemetry_service
    {
        mutable std::unordered_map<uint64_t, std::string> zone_names_;
        mutable std::shared_ptr<spdlog::logger> logger_;
        // Track zone relationships: zone_id -> set of child zones
        mutable std::unordered_map<uint64_t, std::set<uint64_t>> zone_children_;
        // Track zone relationships: zone_id -> parent zone (0 if root)
        mutable std::unordered_map<uint64_t, uint64_t> zone_parents_;

        // Thread safety: shared_mutex allows multiple concurrent readers with exclusive writers
        mutable rpc::shared_mutex zone_names_mutex_;
        mutable rpc::shared_mutex zone_children_mutex_;
        mutable rpc::shared_mutex zone_parents_mutex_;

        // Optional file output
        std::filesystem::path log_directory_;
        std::string test_suite_name_;
        std::string test_name_;
        mutable std::string logger_name_; // Store logger name for proper cleanup

        static constexpr size_t ASYNC_QUEUE_SIZE = 8192;

        std::string get_zone_name(uint64_t zone_id) const;
        std::string get_zone_color(uint64_t zone_id) const;
        std::string get_level_color(level_enum level) const;
        std::string reset_color() const;
        void register_zone_name(
            uint64_t zone_id,
            const std::string& name,
            bool optional_replace) const;
        void init_logger() const;
        void print_topology_diagram() const;
        void print_zone_tree(
            uint64_t zone_id,
            size_t depth) const;

        console_telemetry_service(
            const std::string& test_suite_name,
            const std::string& test_name,
            const std::filesystem::path& directory);

    public:
        static bool create(
            std::shared_ptr<i_telemetry_service>& service,
            const std::string& test_suite_name,
            const std::string& name,
            const std::filesystem::path& directory);

        ~console_telemetry_service() override;
        console_telemetry_service();
        console_telemetry_service(const console_telemetry_service&) = delete;
        console_telemetry_service& operator=(const console_telemetry_service&) = delete;

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
    console_telemetry_service::console_telemetry_service() = default;

    console_telemetry_service::console_telemetry_service(
        const std::string& test_suite_name,
        const std::string& test_name,
        const std::filesystem::path& directory)
        : log_directory_(directory)
        , test_suite_name_(test_suite_name)
        , test_name_(test_name)
    {
    }

    console_telemetry_service::~console_telemetry_service()
    {
        if (logger_)
        {
            // Flush any pending async messages - this blocks until complete
            logger_->flush();

            // Get reference to the thread pool before dropping logger
            auto tp = spdlog::thread_pool();

            // Remove logger from spdlog registry to prevent conflicts
            if (!logger_name_.empty())
            {
                spdlog::drop(logger_name_);
            }

            // Drop the logger reference to allow proper cleanup
            logger_.reset();

            // Wait for thread pool to finish processing if it exists and has work
            if (tp)
            {
                // Wait for the queue to be empty - this is more deterministic than arbitrary sleep
                constexpr int max_wait_ms = 100;
                constexpr int check_interval_ms = 1;

                for (int waited = 0; waited < max_wait_ms; waited += check_interval_ms)
                {
                    if (tp->queue_size() == 0)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
                }

                // Small additional delay to ensure worker thread processes the empty queue check
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    void capitalise(std::string& name)
    {
        // Capitalize the entire name
        for (char& c : name)
        {
            if (c >= 'a' && c <= 'z')
            {
                c = c - 'a' + 'A';
            }
        }
    }

    std::string console_telemetry_service::get_zone_name(uint64_t zone_id) const
    {
        rpc::shared_lock<rpc::shared_mutex> lock(zone_names_mutex_);
        auto it = zone_names_.find(zone_id);
        if (it != zone_names_.end())
        {
            std::string name = it->second;
            // Capitalize the entire name
            capitalise(name);
            return "[" + name + " = " + std::to_string(zone_id) + "]";
        }
        return "[" + std::to_string(zone_id) + "]";
    }

    std::string console_telemetry_service::get_zone_color(uint64_t zone_id) const
    {
        // ANSI color codes - cycle through 8 bright colors
        static const char* colors[] = {
            "\033[91m", // Bright Red
            "\033[92m", // Bright Green
            "\033[93m", // Bright Yellow
            "\033[94m", // Bright Blue
            "\033[95m", // Bright Magenta
            "\033[96m", // Bright Cyan
            "\033[97m", // Bright White
            "\033[90m"  // Bright Black (Gray)
        };
        return colors[zone_id % 8];
    }

    std::string console_telemetry_service::get_level_color(level_enum level) const
    {
        switch (level)
        {
        case warn:
            return "\033[93m"; // Bright Yellow
        case err:
            return "\033[91m"; // Bright Red
        case critical:
            return "\033[95m"; // Bright Magenta
        default:
            return ""; // No color for other levels
        }
    }

    std::string console_telemetry_service::reset_color() const
    {
        return "\033[0m"; // Reset to default color
    }

    void console_telemetry_service::init_logger() const
    {
        if (!logger_)
        {
            if (!log_directory_.empty() && !test_suite_name_.empty() && !test_name_.empty())
            {
                // File output mode - create logger with both console and file sinks
                try
                {
                    // Sanitize test suite name by replacing problematic characters with #
                    auto fixed_suite_name = test_suite_name_;
                    for (auto& ch : fixed_suite_name)
                    {
                        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*')
                            ch = '#';
                    }

                    // Create directories if they don't exist
                    std::error_code ec;
                    auto full_directory_path = log_directory_ / fixed_suite_name;
                    std::filesystem::create_directories(full_directory_path, ec);

                    if (ec)
                    {
                        // Log directory creation failure but continue with console-only logging
                        auto console_logger = spdlog::default_logger();
                        console_logger->warn(
                            "Failed to create console telemetry directory '{}': {} - falling back to console-only mode",
                            full_directory_path.string(),
                            ec.message());
                        logger_ = spdlog::default_logger();
                        return;
                    }

                    // Create log file path
                    auto log_file_path = log_directory_ / fixed_suite_name / (test_name_ + "_console.log");

                    // Use unique logger name based on instance address to avoid conflicts
                    logger_name_ = "console_telemetry_" + std::to_string(reinterpret_cast<uintptr_t>(this));

                    // Create sinks - console sink with colors, file sink without colors
                    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path.string());

                    // Set patterns - console keeps ANSI colors, file uses clean format
                    console_sink->set_pattern("%v");                     // Raw pattern preserves our ANSI formatting
                    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v"); // Clean timestamped format for file

                    // Create logger with both sinks
                    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};
                    logger_ = std::make_shared<spdlog::logger>(logger_name_, sinks.begin(), sinks.end());

                    logger_->set_level(spdlog::level::trace);
                    logger_->flush_on(spdlog::level::trace); // Ensure all messages are written to file

                    // Register with spdlog to avoid name conflicts
                    spdlog::register_logger(logger_);

                    // Log success message to console
                    auto console_logger = spdlog::default_logger();
                    console_logger->info("Console telemetry logging to file: {}", log_file_path.string());
                }
                catch (...)
                {
                    // Fallback to console only if file logging fails
                    logger_ = spdlog::default_logger();
                }
            }
            else
            {
                // Console output mode (default behavior)
                logger_ = spdlog::default_logger();
            }
        }
    }

    void console_telemetry_service::register_zone_name(
        uint64_t zone_id,
        const std::string& name,
        bool optional_replace) const
    {
        std::unique_lock<rpc::shared_mutex> lock(zone_names_mutex_);
        auto it = zone_names_.find(zone_id);
        if (it != zone_names_.end())
        {
            if (it->second != name)
            {
                if (optional_replace)
                {
                    return;
                }
                init_logger();
                logger_->warn("\033[93mWARNING: Zone {} name changing from '{}' to '{}'\033[0m", zone_id, it->second, name);
            }
        }
        zone_names_[zone_id] = name;
    }

    bool console_telemetry_service::create(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        std::shared_ptr<console_telemetry_service> console_service;

        if (!directory.empty())
        {
            // File output mode - use constructor with directory parameters
            console_service = std::shared_ptr<console_telemetry_service>(
                new console_telemetry_service(test_suite_name, name, directory));
        }
        else
        {
            // Console output mode - use default constructor
            console_service = std::make_shared<console_telemetry_service>();
        }

        console_service->init_logger();
        service = console_service;
        return true;
    }

    bool create_console_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        return console_telemetry_service::create(service, test_suite_name, name, directory);
    }

    void console_telemetry_service::on_service_creation(const rpc::telemetry::service_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto parent_zone_id = event.parent_zone_id;
        register_zone_name(zone_id.get_subnet(), name, false);
        init_logger();
        if (parent_zone_id.get_subnet() == 0)
            logger_->info(
                "{}{} service_creation{}",
                get_zone_color(zone_id.get_subnet()),
                get_zone_name(zone_id.get_subnet()),
                reset_color());
        else
            logger_->info(
                "{}{} child_zone_creation: parent={}{}",
                get_zone_color(zone_id.get_subnet()),
                get_zone_name(zone_id.get_subnet()),
                get_zone_name(parent_zone_id.get_subnet()),
                reset_color());

        // Track the parent-child relationship
        {
            std::unique_lock<rpc::shared_mutex> lock(zone_children_mutex_);
            zone_children_[parent_zone_id.get_subnet()].insert(zone_id.get_subnet());
        }
        {
            std::unique_lock<rpc::shared_mutex> lock(zone_parents_mutex_);
            zone_parents_[zone_id.get_subnet()] = parent_zone_id.get_subnet();
        }
        // Print topology diagram after each service creation
        print_topology_diagram();
        return;
    }

    void console_telemetry_service::print_topology_diagram() const
    {
        init_logger();
        logger_->info("{}=== TOPOLOGY DIAGRAM ==={}", get_level_color(level_enum::info), reset_color());

        rpc::shared_lock<rpc::shared_mutex> names_lock(zone_names_mutex_);
        if (zone_names_.empty())
        {
            logger_->info("{}No zones registered yet{}", get_level_color(level_enum::info), reset_color());
            return;
        }

        // Find root zones (zones with no parent)
        std::set<uint64_t> root_zones;
        {
            rpc::shared_lock<rpc::shared_mutex> parents_lock(zone_parents_mutex_);
            for (const auto& zone_pair : zone_names_)
            {
                uint64_t zone_id = zone_pair.first;
                if (zone_parents_.find(zone_id) == zone_parents_.end() || zone_parents_.at(zone_id) == 0)
                {
                    root_zones.insert(zone_id);
                }
            }
        }

        if (root_zones.empty())
        {
            // No parent-child relationships tracked yet, show flat list
            logger_->info("{}Active Zones (no hierarchy tracked yet):{}", get_level_color(level_enum::info), reset_color());
            for (const auto& zone_pair : zone_names_)
            {
                uint64_t zone_id = zone_pair.first;
                const std::string& zone_name = zone_pair.second;
                logger_->info("{}  Zone {}: {}{}", get_zone_color(zone_id), zone_id, zone_name, reset_color());
            }
        }
        else
        {
            // Show hierarchical structure
            logger_->info("{}Zone Hierarchy:{}", get_level_color(level_enum::info), reset_color());
            for (uint64_t root_zone : root_zones)
            {
                print_zone_tree(root_zone, 0);
            }
        }

        logger_->info("{}========================={}", get_level_color(level_enum::info), reset_color());
    }

    void console_telemetry_service::print_zone_tree(
        uint64_t zone_id,
        size_t depth) const
    {
        std::string indent(depth * 2, ' ');
        std::string branch = (depth > 0) ? "├─ " : "";

        std::string zone_name;
        {
            rpc::shared_lock<rpc::shared_mutex> names_lock(zone_names_mutex_);
            auto zone_name_it = zone_names_.find(zone_id);
            zone_name = (zone_name_it != zone_names_.end()) ? zone_name_it->second : "unknown";
        }

        logger_->info(
            "{}{}{}{}Zone {}: {} {}{}",
            get_level_color(level_enum::info),
            indent,
            branch,
            reset_color(),
            get_zone_color(zone_id),
            zone_id,
            zone_name,
            reset_color());

        // Print children
        rpc::shared_lock<rpc::shared_mutex> children_lock(zone_children_mutex_);
        auto children_it = zone_children_.find(zone_id);
        if (children_it != zone_children_.end())
        {
            for (uint64_t child_zone : children_it->second)
            {
                print_zone_tree(child_zone, depth + 1);
            }
        }
    }

    void console_telemetry_service::on_service_deletion(const rpc::telemetry::service_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        init_logger();
        logger_->info(
            "{}{} service_deletion{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_try_cast: destination_zone={} caller_zone={} object_id={} interface_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_add_ref: destination_zone={} object_id={} "
            "caller_zone={} requesting_zone={} options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            get_zone_name(caller_zone_id.get_subnet()),
            get_zone_name(requesting_zone_id.get_subnet()),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_service_release(const rpc::telemetry::service_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_release: destination_zone={} object_id={} caller_zone={} "
            "options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            get_zone_name(caller_zone_id.get_subnet()),
            static_cast<int>(options),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_send(const rpc::telemetry::service_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_send: destination_zone={} caller_zone={} object_id={} interface_id={} method_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_post(const rpc::telemetry::service_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_post: destination_zone={} caller_zone={} object_id={} interface_id={} method_id={} "
            "{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_object_released(const rpc::telemetry::service_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_object_released: destination_zone={} caller_zone={} object_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_transport_down(const rpc::telemetry::service_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} service_transport_down: destination_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_creation(const rpc::telemetry::service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::string uppercase_name(service_proxy_name);
        capitalise(uppercase_name);
        register_zone_name(zone_id.get_subnet(), service_name, true);
        register_zone_name(destination_zone_id.get_subnet(), service_proxy_name, true);
        init_logger();
        logger_->info(
            "{}{} service_proxy_creation: name=[{}] destination_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            uppercase_name,
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_cloned_service_proxy_creation(
        const rpc::telemetry::cloned_service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::string uppercase_name(service_proxy_name);
        capitalise(uppercase_name);
        register_zone_name(zone_id.get_subnet(), service_name, true);
        register_zone_name(destination_zone_id.get_subnet(), service_proxy_name, true);
        init_logger();
        logger_->info(
            "{}{} cloned_service_proxy_creation: name=[{}] destination_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            uppercase_name,
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_deletion(const rpc::telemetry::service_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} service_proxy_deletion: destination_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_try_cast(const rpc::telemetry::service_proxy_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_proxy_try_cast: destination_zone={} caller_zone={} object_id={} interface_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_add_ref(const rpc::telemetry::service_proxy_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_proxy_add_ref: destination_zone={} caller_zone={} "
            "requesting_zone={} object_id={} options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            get_zone_name(requesting_zone_id.get_subnet()),
            object_id.get_val(),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_release(const rpc::telemetry::service_proxy_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_proxy_release: destination_zone={} caller_zone={} object_id={} "
            "options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_add_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} service_proxy_add_external_ref: destination_zone={} "
            "caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_release_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} service_proxy_release_external_ref: destination_zone={} "
            "caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_impl_creation(const rpc::telemetry::impl_creation_event& event) const
    {
        const auto& name = event.name;
        auto address = event.address;
        auto zone_id = event.zone_id;
        init_logger();
        logger_->info(
            "{}{} impl_creation: name={} address={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            name,
            address,
            reset_color());
        return;
    }

    void console_telemetry_service::on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const
    {
        auto address = event.address;
        auto zone_id = event.zone_id;
        init_logger();
        logger_->info(
            "{}{} impl_deletion: address={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            address,
            reset_color());
        return;
    }

    void console_telemetry_service::on_stub_creation(const rpc::telemetry::stub_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto address = event.address;
        init_logger();
        logger_->info(
            "{}{} stub_creation: object_id={} address={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            object_id.get_val(),
            address,
            reset_color());
        return;
    }

    void console_telemetry_service::on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        init_logger();
        logger_->info(
            "{}{} stub_deletion: object_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            object_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_stub_send(const rpc::telemetry::stub_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        init_logger();
        logger_->info(
            "{}{} stub_send: object_id={} interface_id={} method_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} stub_add_ref: object_id={} interface_id={} count={} caller_zone={}{}",
            get_zone_color(destination_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            count,
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_stub_release(const rpc::telemetry::stub_release_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} stub_release: object_id={} interface_id={} count={} caller_zone={}{}",
            get_zone_color(destination_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            count,
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_object_proxy_creation(const rpc::telemetry::object_proxy_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto add_ref_done = event.add_ref_done;
        init_logger();
        logger_->info(
            "{}{} object_proxy_creation: destination_zone={} object_id={} add_ref_done={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            (add_ref_done ? "true" : "false"),
            reset_color());
        return;
    }

    void console_telemetry_service::on_object_proxy_deletion(const rpc::telemetry::object_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        init_logger();
        logger_->info(
            "{}{} object_proxy_deletion: destination_zone={} object_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_interface_proxy_creation(
        const rpc::telemetry::interface_proxy_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        init_logger();
        logger_->info(
            "{}{} interface_proxy_creation: name={} destination_zone={} object_id={} interface_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            name,
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_interface_proxy_deletion(
        const rpc::telemetry::interface_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        init_logger();
        logger_->info(
            "{}{} interface_proxy_deletion: destination_zone={} object_id={} interface_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const
    {
        const auto& method_name = event.method_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        init_logger();
        logger_->info(
            "{}{} interface_proxy_send: method_name={} destination_zone={} object_id={} interface_id={} method_id={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            method_name,
            get_zone_name(destination_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::message(const rpc::log_record& event) const
    {
        auto level = static_cast<level_enum>(event.level);
        const auto& message = event.message;
        const char* level_str;
        switch (level)
        {
        case debug:
            level_str = "DEBUG";
            break;
        case trace:
            level_str = "TRACE";
            break;
        case info:
            level_str = "INFO";
            break;
        case warn:
            level_str = "WARN";
            break;
        case err:
            level_str = "ERROR";
            break;
        case critical:
            level_str = "CRITICAL";
            break;
        case off:
            return;
        default:
            level_str = "UNKNOWN";
            break;
        }

        init_logger();
        std::string level_color = get_level_color(level);
        if (!level_color.empty())
        {
            logger_->info("{}{} {}{}", level_color, level_str, message, reset_color());
        }
        else
        {
            logger_->info("{} {}", level_str, message);
        }
        return;
    }

    void console_telemetry_service::on_transport_creation(const rpc::telemetry::transport_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto status = event.status;
        init_logger();
        logger_->info(
            "{}{} transport_creation: name={} adjacent_zone={} status={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            name,
            get_zone_name(adjacent_zone_id.get_subnet()),
            static_cast<int>(status),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        init_logger();
        logger_->info(
            "{}{} transport_deletion: adjacent_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_status_change(const rpc::telemetry::transport_status_change_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto old_status = event.old_status;
        auto new_status = event.new_status;
        init_logger();
        logger_->info(
            "{}{} transport_status_change: name={} adjacent_zone={} old_status={} new_status={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            name,
            get_zone_name(adjacent_zone_id.get_subnet()),
            static_cast<int>(old_status),
            static_cast<int>(new_status),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_add_destination(const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        init_logger();
        logger_->info(
            "{}{} transport_add_destination: adjacent_zone={} destination={} caller={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            destination.get_subnet(),
            caller.get_subnet(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_remove_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        init_logger();
        logger_->info(
            "{}{} transport_remove_destination: adjacent_zone={} destination={} caller={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            destination.get_subnet(),
            caller.get_subnet(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_accept(const rpc::telemetry::transport_accept_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto result = event.result;
        init_logger();
        logger_->info(
            "{}{}{} transport_accept: adjacent_zone={}{}{} result={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color(),
            get_zone_color(adjacent_zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            reset_color(),
            result,
            reset_color());
        return;
    }

    void console_telemetry_service::on_pass_through_creation(const rpc::telemetry::pass_through_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_count = event.shared_count;
        auto optimistic_count = event.optimistic_count;
        init_logger();
        logger_->info(
            "{}{}{} pass_through_creation: forward_destination={}{}{} reverse_destination={}{}{} shared_count={} "
            "optimistic_count={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color(),
            get_zone_color(forward_destination.get_subnet()),
            get_zone_name(forward_destination.get_subnet()),
            reset_color(),
            get_zone_color(reverse_destination.get_subnet()),
            get_zone_name(reverse_destination.get_subnet()),
            reset_color(),
            shared_count,
            optimistic_count,
            reset_color());
        return;
    }

    void console_telemetry_service::on_pass_through_deletion(const rpc::telemetry::pass_through_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        init_logger();
        logger_->info(
            "{}{}{} pass_through_deletion: forward_destination={}{}{} reverse_destination={}{}{}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color(),
            get_zone_color(forward_destination.get_subnet()),
            get_zone_name(forward_destination.get_subnet()),
            reset_color(),
            get_zone_color(reverse_destination.get_subnet()),
            get_zone_name(reverse_destination.get_subnet()),
            reset_color(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto options = event.options;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        init_logger();
        logger_->info(
            "{}{}{} pass_through_add_ref: forward_destination={}{}{} reverse_destination={}{}{} options={} "
            "shared_delta={} optimistic_delta={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color(),
            get_zone_color(forward_destination.get_subnet()),
            get_zone_name(forward_destination.get_subnet()),
            reset_color(),
            get_zone_color(reverse_destination.get_subnet()),
            get_zone_name(reverse_destination.get_subnet()),
            reset_color(),
            static_cast<int>(options),
            shared_delta,
            optimistic_delta,
            reset_color());
        return;
    }

    void console_telemetry_service::on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        init_logger();
        logger_->info(
            "{}{}{} pass_through_release: forward_destination={}{}{} reverse_destination={}{}{} shared_delta={} "
            "optimistic_delta={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color(),
            get_zone_color(forward_destination.get_subnet()),
            get_zone_name(forward_destination.get_subnet()),
            reset_color(),
            get_zone_color(reverse_destination.get_subnet()),
            get_zone_name(reverse_destination.get_subnet()),
            reset_color(),
            shared_delta,
            optimistic_delta,
            reset_color());
        return;
    }

    void console_telemetry_service::on_pass_through_status_change(
        const rpc::telemetry::pass_through_status_change_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto forward_status = event.forward_status;
        auto reverse_status = event.reverse_status;
        init_logger();
        logger_->info(
            "{}{}{} pass_through_status_change: forward_destination={}{}{} reverse_destination={}{}{} "
            "forward_status={} "
            "reverse_status={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            reset_color(),
            get_zone_color(forward_destination.get_subnet()),
            get_zone_name(forward_destination.get_subnet()),
            reset_color(),
            get_zone_color(reverse_destination.get_subnet()),
            get_zone_name(reverse_destination.get_subnet()),
            reset_color(),
            static_cast<int>(forward_status),
            static_cast<int>(reverse_status),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_proxy_send: dest_zone={} caller_zone={} object={} interface={} method={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_proxy_post: dest_zone={} caller_zone={} object={} interface={} method={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_object_released(
        const rpc::telemetry::service_proxy_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} service_proxy_object_released: dest_zone={} caller_zone={} object={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_service_proxy_transport_down(
        const rpc::telemetry::service_proxy_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} service_proxy_transport_down: dest_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_outbound_send:  adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "interface={} method={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_outbound_post: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "interface={} method={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_outbound_try_cast: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "interface={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_outbound_add_ref: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "requesting_zone={} options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            get_zone_name(requesting_zone_id.get_subnet()),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_release(const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();

        logger_->info(
            "{}{} transport_outbound_release: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_outbound_object_released: adjacent_zone={} dest_zone={} caller_zone={} "
            "object={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_outbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} transport_outbound_transport_down: adjacent_zone={} dest_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_inbound_send: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "interface={} method={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_inbound_post: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "interface={} method={} {}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_inbound_try_cast: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "interface={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            interface_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_inbound_add_ref: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "requesting_zone={} options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            get_zone_name(requesting_zone_id.get_subnet()),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_release(const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_inbound_release: adjacent_zone={} dest_zone={} caller_zone={} object={} "
            "options={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            static_cast<int>(options),

            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        init_logger();
        logger_->info(
            "{}{} transport_inbound_object_released: adjacent_zone={} dest_zone={} caller_zone={} object={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            object_id.get_val(),
            reset_color());
        return;
    }

    void console_telemetry_service::on_transport_inbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        init_logger();
        logger_->info(
            "{}{} transport_inbound_transport_down: adjacent_zone={} dest_zone={} caller_zone={}{}",
            get_zone_color(zone_id.get_subnet()),
            get_zone_name(zone_id.get_subnet()),
            get_zone_name(adjacent_zone_id.get_subnet()),
            get_zone_name(destination_zone_id.get_subnet()),
            get_zone_name(caller_zone_id.get_subnet()),
            reset_color());
        return;
    }
}
