/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <filesystem>
#include <algorithm>
#include <thread>
#include <sstream>
#include <string>

#include <spdlog/spdlog.h>
#include <fmt/os.h>

#include <rpc/internal/polyfill/format.h>
#include <rpc/rpc.h>
#include <rpc/telemetry/telemetry_service_factory.h>

using namespace std::string_literals;

#include <unordered_map>
#include <string>
#include <mutex>
#include <fstream>
#include <filesystem>

// types.h is included via i_telemetry_service.h
#include <rpc/telemetry/i_telemetry_service.h>

namespace rpc::telemetry
{
    class sequence_diagram_telemetry_service : public i_telemetry_service
    {
        struct name_count
        {
            std::string name;
            int count = 0;
        };

        struct zone_object
        {
            rpc::zone zone_id;
            rpc::object object_id;

            bool operator==(const zone_object& other) const
            {
                return zone_id == other.zone_id && object_id == other.object_id;
            }
        };

        struct zone_object_hash
        {
            std::size_t operator()(zone_object const& s) const noexcept
            {
                std::size_t h1 = std::hash<uint64_t>{}(s.zone_id.get_subnet());
                std::size_t h2 = std::hash<uint64_t>{}(s.object_id.get_val());
                return h1 ^ (h2 << 1); // or use boost::hash_combine
            }
        };

        struct orig_zone
        {
            rpc::zone zone_id;
            rpc::destination_zone destination_zone_id;
            rpc::caller_zone caller_zone_id;
            bool operator==(const orig_zone& other) const
            {
                return zone_id == other.zone_id && destination_zone_id == other.destination_zone_id
                       && caller_zone_id == other.caller_zone_id;
            }
        };

        struct orig_zone_hash
        {
            std::size_t operator()(orig_zone const& s) const noexcept
            {
                std::size_t h1 = std::hash<uint64_t>{}(s.zone_id.get_subnet());
                std::size_t h2 = std::hash<uint64_t>{}(s.destination_zone_id.get_subnet());
                std::size_t h3 = std::hash<uint64_t>{}(s.caller_zone_id.get_subnet());
                return h1 ^ (h2 << 1) ^ (h3 << 2); // or use boost::hash_combine
            }
        };

        struct interface_proxy_id
        {
            bool operator==(const interface_proxy_id& other) const
            {
                return zone_id == other.zone_id && destination_zone_id == other.destination_zone_id
                       && object_id == other.object_id && interface_id == other.interface_id;
            }
            rpc::zone zone_id;
            rpc::destination_zone destination_zone_id;
            rpc::object object_id;
            rpc::interface_ordinal interface_id;
        };

        struct interface_proxy_id_hash
        {
            std::size_t operator()(interface_proxy_id const& s) const noexcept
            {
                std::size_t h1 = std::hash<uint64_t>{}(s.zone_id.get_subnet());
                std::size_t h2 = std::hash<uint64_t>{}(s.destination_zone_id.get_subnet());
                std::size_t h3 = std::hash<uint64_t>{}(s.object_id.get_val());
                return h1 ^ (h2 << 1) ^ (h3 << 2); // or use boost::hash_combine
            }
        };

        struct impl
        {
            rpc::zone zone_id;
            std::string name;
            uint_fast64_t count;
        };

        struct stub_info
        {
            uint64_t address = 0;
            uint64_t count = 0;
        };

        mutable std::mutex mux;
        mutable std::unordered_map<rpc::zone, name_count> services;
        mutable std::unordered_map<orig_zone, name_count, orig_zone_hash> service_proxies;
        mutable std::unordered_map<uint64_t, rpc::zone> historical_impls;
        mutable std::unordered_map<uint64_t, impl> impls;
        mutable std::unordered_map<zone_object, stub_info, zone_object_hash> stubs;
        mutable std::unordered_map<interface_proxy_id, name_count, interface_proxy_id_hash> interface_proxies;
        mutable std::unordered_map<interface_proxy_id, uint64_t, interface_proxy_id_hash> object_proxies;

        FILE* output_ = nullptr;

        sequence_diagram_telemetry_service(FILE* output);

        void add_new_object(
            const std::string& name,
            uint64_t address,
            rpc::zone zone_id) const;

    public:
        static bool create(
            std::shared_ptr<i_telemetry_service>& service,
            const std::string& test_suite_name,
            const std::string& name,
            const std::filesystem::path& directory);

        ~sequence_diagram_telemetry_service() override;

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
    std::string get_thread_id()
    {
        std::stringstream ssr;
        ssr << std::this_thread::get_id();
        return ssr.str();
    }

    bool sequence_diagram_telemetry_service::create(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        auto fixed_name = test_suite_name;
        for (auto& ch : fixed_name)
        {
            if (ch == '/')
                ch = '#';
        }
        std::error_code ec;
        std::filesystem::create_directories(directory / fixed_name, ec);

        auto file_name = directory / fixed_name / (name + ".pu");
        std::string fn = file_name.string();

#ifndef _MSC_VER
        FILE* output = ::fopen(fn.c_str(), "w+");
#else
        FILE* output;
        auto err = ::fopen_s(&output, fn.c_str(), "w+");
        if (!err)
            return false;
#endif
        if (!output)
            return false;

        fmt::println(output, "@startuml");
        fmt::println(output, "title {}.{}", test_suite_name, name);

        service = std::shared_ptr<sequence_diagram_telemetry_service>(new sequence_diagram_telemetry_service(output));
        return true;
    }

    bool create_sequence_diagram_telemetry_service(
        std::shared_ptr<i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        return sequence_diagram_telemetry_service::create(service, test_suite_name, name, directory);
    }

    sequence_diagram_telemetry_service::sequence_diagram_telemetry_service(FILE* output)
        : output_(output)
    {
    }

    sequence_diagram_telemetry_service::~sequence_diagram_telemetry_service()
    {
        fmt::println(output_, "note left");
        fmt::println(output_, "orphaned services {}", services.size());
        fmt::println(output_, "orphaned impls {}", impls.size());
        fmt::println(output_, "orphaned stubs {}", stubs.size());
        fmt::println(output_, "orphaned service_proxies {}", service_proxies.size());
        fmt::println(output_, "orphaned interface_proxies {}", interface_proxies.size());
        fmt::println(output_, "orphaned object_proxies {}", object_proxies.size());

        std::for_each(
            services.begin(),
            services.end(),
            [this](std::pair<rpc::zone, name_count> const& it)
            {
                fmt::println(
                    output_,
                    "error service zone_id {} service {} count {}",
                    it.first.get_subnet(),
                    it.second.name,
                    it.second.count);
            });
        std::for_each(
            impls.begin(),
            impls.end(),
            [this](std::pair<uint64_t, impl> const& it)
            {
                fmt::println(
                    output_,
                    "error implementation {} zone_id {} count {}",
                    it.second.name,
                    it.second.zone_id.get_subnet(),
                    it.second.count);
            });
        std::for_each(
            stubs.begin(),
            stubs.end(),
            [this](std::pair<zone_object, stub_info> const& it)
            {
                fmt::println(
                    output_,
                    "error stub zone_id {} object_id {} count {} address {}",
                    it.first.zone_id.get_subnet(),
                    it.first.object_id.get_val(),
                    it.second.count,
                    it.second.address);
            });
        std::for_each(
            service_proxies.begin(),
            service_proxies.end(),
            [this](std::pair<orig_zone, name_count> const& it)
            {
                fmt::println(
                    output_,
                    "error service proxy zone_id {} destination_zone_id {} caller_id {} name {} count {}",
                    it.first.zone_id.get_subnet(),
                    it.first.destination_zone_id.get_subnet(),
                    it.first.caller_zone_id.get_subnet(),
                    it.second.name,
                    it.second.count);
            });
        std::for_each(
            object_proxies.begin(),
            object_proxies.end(),
            [this](std::pair<interface_proxy_id, uint64_t> const& it)
            {
                fmt::println(
                    output_,
                    "error object_proxy zone_id {} destination_zone_id {} object_id {} count {}",
                    it.first.zone_id.get_subnet(),
                    it.first.destination_zone_id.get_subnet(),
                    it.first.object_id.get_val(),
                    it.second);
            });
        std::for_each(
            interface_proxies.begin(),
            interface_proxies.end(),
            [this](std::pair<interface_proxy_id, name_count> const& it)
            {
                fmt::println(
                    output_,
                    "error interface_proxy {} zone_id {} destination_zone_id {} object_id {} count {}",
                    it.second.name,
                    it.first.zone_id.get_subnet(),
                    it.first.destination_zone_id.get_subnet(),
                    it.first.object_id.get_val(),
                    it.second.count);
            });

        bool is_heathy = services.empty() && service_proxies.empty() && impls.empty() && stubs.empty()
                         && interface_proxies.empty() && object_proxies.empty();
        if (is_heathy)
        {
            fmt::println(output_, "system is healthy");
        }
        else
        {
            fmt::println(output_, "error system is NOT healthy!");
        }
        fmt::println(output_, "end note");
        fmt::println(output_, "@enduml");
        fclose(output_);
        output_ = 0;
        historical_impls.clear();
    }

    std::string service_alias(rpc::zone zone_id)
    {
        return rpc::format("s{}", zone_id.get_subnet());
    }

    uint64_t service_order(rpc::zone zone_id)
    {
        return std::min((uint32_t)(zone_id.get_subnet() * 100000), (uint32_t)999999);
    }

    std::string object_stub_alias(
        rpc::zone zone_id,
        rpc::object object_id)
    {
        return rpc::format("os_{}_{}", zone_id.get_subnet(), object_id.get_val());
    }

    uint64_t object_stub_order(
        rpc::zone zone_id,
        rpc::object object_id)
    {
        return service_order(zone_id) + object_id.get_val();
    }

    std::string object_alias(uint64_t address)
    {
        return rpc::format("o_{}", address);
    }

    uint64_t object_order(
        rpc::zone zone_id,
        uint64_t address)
    {
        return service_order(zone_id) + address % 100;
    }

    std::string object_proxy_alias(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id)
    {
        return rpc::format("op_{}_{}_{}", zone_id.get_subnet(), destination_zone_id.get_subnet(), object_id.get_val());
    }

    uint64_t object_proxy_order(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id)
    {
        return service_order(zone_id) + destination_zone_id.get_subnet() * 1000 + object_id.get_val();
    }

    std::string service_proxy_alias(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id)
    {
        return rpc::format(
            "sp{}_{}_{}", zone_id.get_subnet(), destination_zone_id.get_subnet(), caller_zone_id.get_subnet());
    }

    uint64_t service_proxy_order(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id)
    {
        return service_order(zone_id) + destination_zone_id.get_subnet() * 10000;
    }

    void sequence_diagram_telemetry_service::on_service_creation(const rpc::telemetry::service_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto parent_zone_id = event.parent_zone_id;
        std::ignore = parent_zone_id;
        std::lock_guard g(mux);
        auto entry = services.find(zone_id);
        if (entry == services.end())
        {
            services.emplace(zone_id, name_count{name, 1});
            fmt::println(
                output_,
                "participant \"{} zone {}\" as {} order {} #Moccasin",
                name,
                zone_id.get_subnet(),
                service_alias(zone_id),
                service_order(zone_id));
            fmt::println(output_, "activate {} #Moccasin", service_alias(zone_id));
        }
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_deletion(const rpc::telemetry::service_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        std::lock_guard g(mux);
        auto found = services.find(zone_id);
        if (found == services.end())
        {
            spdlog::error("service not found zone_id {}", zone_id.get_subnet());
        }
        else
        {
            auto count = --found->second.count;
            if (count == 0)
            {
                services.erase(found);
                fmt::println(output_, "deactivate {}", service_alias(zone_id));
            }
            else
            {
                spdlog::error("service still being used! name {} zone_id {}", found->second.name, service_alias(zone_id));
                fmt::println(output_, "deactivate {}", service_alias(zone_id));
                fmt::println(output_, "hnote over s{} #red : (still being used!)", service_alias(zone_id));
            }
        }
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        if (zone_id == destination_zone_id)
            fmt::println(
                output_,
                "{} -> {} : try_cast {}",
                service_alias(zone_id),
                object_proxy_alias({zone_id}, destination_zone_id, object_id),
                interface_id.get_val());
        else
            fmt::println(
                output_,
                "{} -> {} : try_cast {}",
                service_alias(zone_id),
                service_proxy_alias({zone_id}, destination_zone_id, caller_zone_id),
                interface_id.get_val());
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = caller_zone_id;
        std::ignore = requesting_zone_id;
        std::ignore = options;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        auto dest = destination_zone_id;

        if (rpc::add_ref_options::normal == options)
        {
            if (zone_id != dest)
            {
                fmt::println(
                    output_,
                    "{} -> {} : add_ref",
                    service_alias(zone_id),
                    service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
            }
            else if (object_id != rpc::dummy_object_id)
            {
                //     fmt::println(output_, "{} -> {} : dummy add_ref", service_alias({caller_zone_id.get_subnet()}),
                //     object_stub_alias(zone_id, object_id));
                // }
                // else
                // {
                fmt::println(output_, "{} -> {} : add_ref", service_alias(zone_id), object_stub_alias(zone_id, object_id));
            }
        }
        else if (!!(options & rpc::add_ref_options::build_caller_route)
                 && !!(options & rpc::add_ref_options::build_destination_route))
        {
            if (zone_id != dest)
            {
                fmt::println(
                    output_,
                    "{} -->x {} : add_ref delegate linking",
                    service_alias(caller_zone_id.get_address()),
                    service_alias(zone_id));
            }
            else
            {
                fmt::println(
                    output_,
                    "{} -> {} : add_ref delegate linking",
                    service_alias(zone_id),
                    object_stub_alias(zone_id, object_id));
            }
        }
        else
        {
            if (!!(options & rpc::add_ref_options::build_destination_route))
            {
                if (zone_id != dest)
                {
                    fmt::println(
                        output_,
                        "{} -[#green]>o {} : add_ref build destination",
                        service_alias(zone_id),
                        service_alias(destination_zone_id));
                }
                else
                {
                    fmt::println(
                        output_,
                        "{} -> {} : add_ref build destination",
                        service_alias(zone_id),
                        object_stub_alias(zone_id, object_id));
                }
            }
            if (!!(options & rpc::add_ref_options::build_caller_route))
            {
                if (zone_id != dest)
                {
                    fmt::println(
                        output_,
                        "{} o-[#magenta]> {} : add_ref build caller {}",
                        service_alias(caller_zone_id.get_address()),
                        service_alias(zone_id),
                        get_thread_id());
                }
                else
                {
                    fmt::println(
                        output_,
                        "{} -> {} : add_ref build caller {}",
                        service_alias(zone_id),
                        object_stub_alias(zone_id, object_id),
                        get_thread_id());
                }
            }
        }
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_release(const rpc::telemetry::service_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = caller_zone_id;
        std::ignore = options;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        auto dest = destination_zone_id;

        if (zone_id != dest)
        {
            fmt::println(
                output_, "{} -> {} : release", service_alias(zone_id), service_proxy_alias(zone_id, dest, caller_zone_id));
        }
        else if (object_id != rpc::dummy_object_id)
        {
            //     fmt::println(output_, "hnote over {} : dummy release", service_alias(zone_id));
            // }
            // else
            // {
            fmt::println(output_, "{} -> {} : release", service_alias(zone_id), object_stub_alias(zone_id, object_id));
        }
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_creation(
        const rpc::telemetry::service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = service_name;
        std::ignore = service_proxy_name;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::string route_name;
        std::string destination_name;

        auto search = services.find(destination_zone_id);
        if (search != services.end())
            destination_name = search->second.name;
        else
            destination_name = std::to_string(destination_zone_id);

        if (zone_id == caller_zone_id)
        {
            route_name = "To:"s + destination_name;
        }
        else
        {
            std::string caller_name;
            search = services.find(caller_zone_id.get_address());
            if (search != services.end())
                caller_name = search->second.name;
            else
                caller_name = std::to_string(caller_zone_id);
            route_name = "channel from "s + caller_name + " to " + destination_name;
        }

        fmt::println(
            output_,
            "participant \"{} \\nzone {} \\ndestination {} \\ncaller {}\" as {} order {} #cyan",
            route_name,
            zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_proxy_order(zone_id, destination_zone_id));
        // fmt::println(output_, "{} --> {} : links to", service_proxy_alias(zone_id, destination_zone_id,
        // caller_zone_id), service_alias(destination_zone_id));
        fmt::println(output_, "activate {} #cyan", service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
        std::lock_guard g(mux);
        auto found = service_proxies.find(orig_zone{zone_id, destination_zone_id, caller_zone_id});
        if (found == service_proxies.end())
        {
            service_proxies.emplace(orig_zone{zone_id, destination_zone_id, caller_zone_id}, name_count{service_name, 0});
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_cloned_service_proxy_creation(
        const rpc::telemetry::cloned_service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = service_name;
        std::ignore = service_proxy_name;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::string route_name;
        std::string destination_name;

        auto search = services.find(destination_zone_id);
        if (search != services.end())
            destination_name = search->second.name;
        else
            destination_name = std::to_string(destination_zone_id);

        if (zone_id == caller_zone_id)
        {
            route_name = "To:"s + destination_name;
        }
        else
        {
            std::string caller_name;
            search = services.find(caller_zone_id.get_address());
            if (search != services.end())
                caller_name = search->second.name;
            else
                caller_name = std::to_string(caller_zone_id);
            route_name = "channel from "s + caller_name + " to " + destination_name;
        }

        fmt::println(
            output_,
            "participant \"{} \\nzone {} \\ndestination {} \\ncaller {}\" as {} order {} #cyan",
            route_name,
            zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_proxy_order(zone_id, destination_zone_id));
        // fmt::println(output_, "{} --> {} : links to", service_proxy_alias(zone_id, destination_zone_id,
        // caller_zone_id), service_alias(destination_zone_id));
        fmt::println(output_, "activate {} #cyan", service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
        std::lock_guard g(mux);
        auto found = service_proxies.find(orig_zone{zone_id, destination_zone_id, caller_zone_id});
        if (found == service_proxies.end())
        {
            service_proxies.emplace(orig_zone{zone_id, destination_zone_id, caller_zone_id}, name_count{route_name, 0});
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_deletion(
        const rpc::telemetry::service_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        std::string name;
        int count = 0;

        auto found = service_proxies.find(orig_zone{zone_id, destination_zone_id, caller_zone_id});
        if (found == service_proxies.end())
        {
            spdlog::warn(
                "service_proxy not found zone_id {} destination_zone_id {} caller_zone_id {}",
                zone_id.get_subnet(),
                destination_zone_id.get_subnet(),
                caller_zone_id.get_subnet());
        }
        else
        {
            name = found->second.name;
            count = found->second.count;
            if (count == 0)
            {
                service_proxies.erase(found);
                fmt::println(output_, "deactivate {}", service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
                fmt::println(
                    output_, "hnote over {} : deleted", service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
            }
        }

        if (count)
        {
            spdlog::error(
                "service still being used! name {} zone_id {} destination_zone_id {} caller_zone_id {}",
                name,
                zone_id.get_subnet(),
                destination_zone_id.get_subnet(),
                caller_zone_id.get_subnet());
            fmt::println(output_, "deactivate {}", service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
            fmt::println(
                output_,
                "hnote over {} #red : deleted (still being used!)",
                service_proxy_alias(zone_id, destination_zone_id, caller_zone_id));
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_try_cast(
        const rpc::telemetry::service_proxy_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        fmt::println(
            output_,
            "{} -> {} : try_cast",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_alias(zone_id));
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_add_ref(
        const rpc::telemetry::service_proxy_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = caller_zone_id;
        std::ignore = requesting_zone_id;
        std::ignore = options;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::string type;
        if (!!(options & rpc::add_ref_options::build_caller_route)
            && !!(options & rpc::add_ref_options::build_destination_route))
        {
            type = "delegate linking";
        }
        else
        {
            if (!!(options & rpc::add_ref_options::build_destination_route))
            {
                type = "build destination";
            }
            if (!!(options & rpc::add_ref_options::build_caller_route))
            {
                type = "build caller";
            }
        }

        // std::lock_guard g(mux);
        // auto found = service_proxies.find(orig_zone{zone_id, destination_zone_id, caller_zone_id});
        // if(found == service_proxies.end())
        // {
        //     //service_proxies.emplace(orig_zone{zone_id, destination_zone_id, caller_zone_id}, name_count{name, 1});
        //     fmt::println(output_, "object add_ref name with proxy added {} zone_id {} destination_zone_id {}
        //     caller_zone_id {}", name, zone_id.get_subnet(), destination_zone_id.get_subnet(), caller_zone_id.get_subnet());
        // }
        // else
        // {
        //     fmt::println(output_, "object add_ref name {} zone_id {} destination_zone_id {} caller_zone_id {}
        //     object_id {}", name, zone_id.get_subnet(), destination_zone_id.get_subnet(), caller_zone_id.get_subnet(),
        //     object_id.get_val());
        // }
        if (object_id == rpc::dummy_object_id)
        {
            fmt::println(
                output_,
                "{} -> {} : dummy add_ref {} {}",
                service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
                service_alias(destination_zone_id),
                type,
                get_thread_id());
        }
        else
        {
            fmt::println(
                output_,
                "{} -> {} : add_ref {} {}",
                service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
                service_alias(destination_zone_id),
                type,
                get_thread_id());
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_release(
        const rpc::telemetry::service_proxy_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = caller_zone_id;
        std::ignore = options;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        auto dest = destination_zone_id;

        if (object_id == rpc::dummy_object_id)
        {
            fmt::println(
                output_,
                "{} -> {} : dummy release {}",
                service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
                service_alias(dest),
                get_thread_id());
        }
        else
        {
            fmt::println(
                output_,
                "{} -> {} : release {}",
                service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
                service_alias(dest),
                get_thread_id());
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_add_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        auto found = service_proxies.find(orig_zone{zone_id, destination_zone_id, caller_zone_id});
        if (found == service_proxies.end())
        {
            spdlog::error(
                "service_proxy add_external_ref not found zone_id {} destination_zone_id {} caller_zone_id {}",
                zone_id.get_subnet(),
                destination_zone_id.get_subnet(),
                caller_zone_id.get_subnet());
        }
        else
        {
            ++found->second.count;
        }

        auto entry = services.find(destination_zone_id);
        if (entry == services.end())
        {
            services.emplace(destination_zone_id, name_count{"", 1});
            fmt::println(
                output_,
                "participant \"zone {}\" as {} order {} #Moccasin",
                destination_zone_id.get_subnet(),
                service_alias(destination_zone_id),
                service_order(destination_zone_id));
            fmt::println(output_, "activate {} #Moccasin", service_alias(destination_zone_id));
        }
        fmt::println(
            output_,
            "hnote over {} : add_external_ref {}",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            get_thread_id());
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_release_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        auto found = service_proxies.find(orig_zone{zone_id, destination_zone_id, caller_zone_id});
        if (found == service_proxies.end())
        {
            spdlog::error(
                "service_proxy release_external_ref not found zone_id {} destination_zone_id {} caller_zone_id {}",
                zone_id.get_subnet(),
                destination_zone_id.get_subnet(),
                caller_zone_id.get_subnet());
        }

        fmt::println(
            output_,
            "hnote over {} : release_external_ref {}",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            get_thread_id());
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::add_new_object(
        const std::string& name,
        uint64_t address,
        rpc::zone zone_id) const
    {
        auto found = impls.find(address);
        if (found == impls.end())
        {
            impls.emplace(address, impl{zone_id, name, 1});
            historical_impls[address] = zone_id;
        }
        else
        {
            if (found->second.zone_id != zone_id)
            {
                fmt::println("[ warn  ] object being registered in two zones");
            }
            else
            {
                return;
            }
        }

        fmt::println(
            output_, "participant \"{}\" as {} order {}", name, object_alias(address), object_order(zone_id, address));
        fmt::println(output_, "activate {}", object_alias(address));
    }

    void sequence_diagram_telemetry_service::on_impl_creation(const rpc::telemetry::impl_creation_event& event) const
    {
        const auto& name = event.name;
        auto address = event.address;
        auto zone_id = event.zone_id;
        std::lock_guard g(mux);
        if (historical_impls.find(address) != historical_impls.end())
        {
            RPC_ERROR("historical address reused");
            RPC_ASSERT(false);
        }
        add_new_object(name, address, zone_id);
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const
    {
        auto address = event.address;
        auto zone_id = event.zone_id;
        std::ignore = zone_id;

        std::lock_guard g(mux);
        auto found = impls.find(address);
        if (found == impls.end())
        {
            spdlog::error("impl not found interface_id {}", address);
        }
        else
        {
            impls.erase(found);
        }
        historical_impls.erase(address);
        fmt::println(output_, "deactivate {}", object_alias(address));
        fmt::println(output_, "hnote over {} : deleted", object_alias(address));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_stub_creation(const rpc::telemetry::stub_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto address = event.address;
        std::lock_guard g(mux);

        add_new_object("unknown", address, zone_id);
        stubs.emplace(zone_object{zone_id, object_id}, stub_info{address, 0});
#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        fmt::println(
            output_,
            "participant \"object stub\\nzone {}\\nobject {}\" as {} order {} #lime",
            zone_id.get_subnet(),
            object_id.get_val(),
            object_stub_alias(zone_id, object_id),
            object_stub_order(zone_id, object_id));
        fmt::println(output_, "activate {} #lime", object_stub_alias(zone_id, object_id));
        fmt::println(output_, "{} --> {} : links to", object_stub_alias(zone_id, object_id), object_alias(address));
#endif
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        std::lock_guard g(mux);
        auto found = stubs.find(zone_object{zone_id, object_id});
        if (found == stubs.end())
        {
            spdlog::error("stub not found zone_id {}", zone_id.get_subnet());
        }
#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        else
        {
            if (found->second.count == 0)
            {
                fmt::println(output_, "deactivate {}", object_stub_alias(zone_id, object_id));
                stubs.erase(found);
            }
            else
            {
                spdlog::error(
                    "stub still being used! zone_id {} object id {} address {}",
                    zone_id.get_subnet(),
                    object_id.get_val(),
                    found->second.address);
                fmt::println(output_, "deactivate {}", object_stub_alias(zone_id, object_id));
            }
            fmt::println(output_, "hnote over {} : deleted", object_stub_alias(zone_id, object_id));
        }
#else
        stubs.erase(found);
#endif
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_stub_send(const rpc::telemetry::stub_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        fmt::println(output_, "note over {} : send", object_stub_alias(zone_id, object_id));
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = interface_id;
        std::ignore = object_id;
        std::ignore = count;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        auto found = stubs.find(zone_object{destination_zone_id, object_id});
        if (found == stubs.end())
        {
            spdlog::error(
                "stub not found zone_id {} caller_zone_id {} object_id {}",
                destination_zone_id.get_subnet(),
                caller_zone_id.get_subnet(),
                object_id.get_val());
        }
        else
        {
            found->second.count++;
            fmt::println(
                output_, "hnote over {} : begin add_ref count {} ", object_stub_alias(destination_zone_id, object_id), count);
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_stub_release(const rpc::telemetry::stub_release_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = count;
        std::ignore = caller_zone_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        auto found = stubs.find(zone_object{destination_zone_id, object_id});
        if (found == stubs.end())
        {
            spdlog::error(
                "stub not found zone_id {} caller_zone_id {} object_id {}",
                destination_zone_id.get_subnet(),
                caller_zone_id.get_subnet(),
                object_id.get_val());
        }
        {
            auto new_count = --found->second.count;
            if (new_count == 0)
            {
                fmt::println(
                    output_, "hnote over {} : release count {}", object_stub_alias(destination_zone_id, object_id), count);
            }
            else
            {
                fmt::println(
                    output_, "hnote over {} : release count {}", object_stub_alias(destination_zone_id, object_id), count);
            }
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_object_proxy_creation(
        const rpc::telemetry::object_proxy_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto add_ref_done = event.add_ref_done;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = add_ref_done;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING

        std::lock_guard g(mux);
        object_proxies.emplace(interface_proxy_id{zone_id, destination_zone_id, object_id, {0}}, 1);
        fmt::println(
            output_,
            "participant \"object_proxy\\nzone {}\\ndestination {}\\nobject {}\" as {} order {} #pink",
            zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            object_id.get_val(),
            object_proxy_alias(zone_id, destination_zone_id, object_id),
            object_proxy_order(zone_id, destination_zone_id, object_id));
        fmt::println(output_, "activate {} #pink", object_proxy_alias(zone_id, destination_zone_id, object_id));
        fmt::println(
            output_,
            "{} --> {} : links to",
            object_proxy_alias(zone_id, destination_zone_id, object_id),
            object_stub_alias(destination_zone_id, object_id));
        if (add_ref_done)
        {
            fmt::println(
                output_,
                "{} -> {} : complete_add_ref",
                object_proxy_alias(zone_id, destination_zone_id, object_id),
                service_proxy_alias(zone_id, destination_zone_id, zone_id));
        }
        else
        {
            fmt::println(
                output_,
                "{} -> {} : add_ref",
                object_proxy_alias(zone_id, destination_zone_id, object_id),
                service_proxy_alias(zone_id, destination_zone_id, zone_id));
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_object_proxy_deletion(
        const rpc::telemetry::object_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        auto found = object_proxies.find(interface_proxy_id{zone_id, destination_zone_id, object_id, {0}});
        if (found == object_proxies.end())
        {
            spdlog::error(
                "rpc::object proxy not found zone_id {} destination_zone_id {} object_id {}",
                zone_id.get_subnet(),
                destination_zone_id.get_subnet(),
                object_id.get_val());
        }
        else
        {
            auto count = --found->second;
            if (count == 0)
            {
                object_proxies.erase(found);
                fmt::println(output_, "deactivate {}", object_proxy_alias(zone_id, destination_zone_id, object_id));
                fmt::println(
                    output_,
                    "{} -> {} : release",
                    object_proxy_alias(zone_id, destination_zone_id, object_id),
                    service_proxy_alias(zone_id, destination_zone_id, zone_id));
            }
            else
            {
                spdlog::error(
                    "rpc::object proxy still being used! zone_id {} destination_zone_id {} object_id {}",
                    zone_id.get_subnet(),
                    destination_zone_id.get_subnet(),
                    object_id.get_val());
                fmt::println(output_, "deactivate {}", object_proxy_alias(zone_id, destination_zone_id, object_id));
            }
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_interface_proxy_creation(
        const rpc::telemetry::interface_proxy_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        std::ignore = name;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        interface_proxies.emplace(
            interface_proxy_id{zone_id, destination_zone_id, object_id, interface_id}, name_count{name, 1});
        fmt::println(
            output_,
            "hnote over {} : new interface proxy \\n {}",
            object_proxy_alias(zone_id, destination_zone_id, object_id),
            name);
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_interface_proxy_deletion(
        const rpc::telemetry::interface_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        std::lock_guard g(mux);
        auto found = interface_proxies.find(interface_proxy_id{zone_id, destination_zone_id, object_id, interface_id});
        if (found == interface_proxies.end())
        {
            spdlog::warn(
                "interface proxy not found zone_id {} destination_zone_id {} object_id {}",
                zone_id.get_subnet(),
                destination_zone_id.get_subnet(),
                object_id.get_val());
        }
        else
        {
            auto count = --found->second.count;
            if (count == 0)
            {
                fmt::println(
                    output_,
                    "hnote over {} : deleted \\n {} ",
                    object_proxy_alias(zone_id, destination_zone_id, object_id),
                    found->second.name);
                interface_proxies.erase(found);
            }
            else
            {
                spdlog::error(
                    "interface proxy still being used! name {} zone_id {} destination_zone_id {} object_id {}",
                    found->second.name,
                    zone_id.get_subnet(),
                    destination_zone_id.get_subnet(),
                    object_id.get_val());
                fmt::println(
                    output_,
                    "hnote over {} : deleted \\n {}",
                    object_proxy_alias(zone_id, destination_zone_id, object_id),
                    found->second.name);
            }
        }
        fflush(output_);
#endif
        return;
    }

    void sequence_diagram_telemetry_service::on_interface_proxy_send(
        const rpc::telemetry::interface_proxy_send_event& event) const
    {
        const auto& method_name = event.method_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = method_name;
        std::ignore = zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;

#ifdef CANOPY_USE_TELEMETRY_RAII_LOGGING
        fmt::println(
            output_,
            "{} -> {} : {}",
            object_proxy_alias(zone_id, destination_zone_id, object_id),
            object_stub_alias(destination_zone_id, object_id),
            method_name);
#else
        auto ob = stubs.find({destination_zone_id, object_id});
        if (ob != stubs.end())
        {
            fmt::println(output_, "{} -> {} : {}", service_alias(zone_id), object_alias(ob->second.address), method_name);
        }
        else
        {
            fmt::println(output_, "{} -> {} : {}", service_alias(zone_id), service_alias(destination_zone_id), method_name);
        }
#endif
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::message(const rpc::log_record& event) const
    {
        auto level = static_cast<level_enum>(event.level);
        const auto& message = event.message;
        std::string colour;
        switch (level)
        {
        case debug:
            colour = "green";
            break;
        case trace:
            colour = "blue";
            break;
        case info:
            colour = "honeydew";
            break;
        case warn:
            colour = "orangered";
            break;
        case err:
            colour = "red";
            break;
        case off:
            return;
        case critical:
        default:
            colour = "red";
            break;
        }
        fmt::println(output_, "note left #{}: {} {}", colour, message, get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_creation(const rpc::telemetry::transport_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto status = event.status;
        fmt::println(
            output_,
            "note over zone_{}: transport_creation: name={} adjacent_zone={} status={}",
            zone_id.get_subnet(),
            name,
            adjacent_zone_id.get_subnet(),
            static_cast<int>(status));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        fmt::println(
            output_,
            "note over zone_{}: transport_deletion: adjacent_zone={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_status_change(
        const rpc::telemetry::transport_status_change_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto old_status = event.old_status;
        auto new_status = event.new_status;
        fmt::println(
            output_,
            "note over zone_{}: transport_status_change: name={} adjacent_zone={} old_status={} new_status={}",
            zone_id.get_subnet(),
            name,
            adjacent_zone_id.get_subnet(),
            static_cast<int>(old_status),
            static_cast<int>(new_status));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_add_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        fmt::println(
            output_,
            "note over zone_{}: transport_add_destination: adjacent_zone={} destination={} caller={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination.get_subnet(),
            caller.get_subnet());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_remove_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        fmt::println(
            output_,
            "note over zone_{}: transport_remove_destination: adjacent_zone={} destination={} caller={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination.get_subnet(),
            caller.get_subnet());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_accept(const rpc::telemetry::transport_accept_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto result = event.result;
        fmt::println(
            output_,
            "note over zone_{}: transport_accept: adjacent_zone={} result={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            result);
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_pass_through_creation(
        const rpc::telemetry::pass_through_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_count = event.shared_count;
        auto optimistic_count = event.optimistic_count;
        fmt::println(
            output_,
            "note over pass_through: pass_through_creation: zone={} forward_destination={} reverse_destination={} "
            "shared_count={} optimistic_count={}",
            zone_id.get_subnet(),
            forward_destination.get_subnet(),
            reverse_destination.get_subnet(),
            shared_count,
            optimistic_count);
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_pass_through_deletion(
        const rpc::telemetry::pass_through_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        fmt::println(
            output_,
            "note over pass_through: pass_through_deletion: zone={} forward_destination={} reverse_destination={}",
            zone_id.get_subnet(),
            forward_destination.get_subnet(),
            reverse_destination.get_subnet());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_pass_through_add_ref(
        const rpc::telemetry::pass_through_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto options = event.options;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        fmt::println(
            output_,
            "note over pass_through: pass_through_add_ref: zone={} forward_destination={} reverse_destination={} "
            "options={} "
            "shared_delta={} optimistic_delta={}",
            zone_id.get_subnet(),
            forward_destination.get_subnet(),
            reverse_destination.get_subnet(),
            static_cast<int>(options),
            shared_delta,
            optimistic_delta);
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_pass_through_release(
        const rpc::telemetry::pass_through_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        fmt::println(
            output_,
            "note over pass_through: pass_through_release: zone={} forward_destination={} reverse_destination={} "
            "shared_delta={} optimistic_delta={}",
            zone_id.get_subnet(),
            forward_destination.get_subnet(),
            reverse_destination.get_subnet(),
            shared_delta,
            optimistic_delta);
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_pass_through_status_change(
        const rpc::telemetry::pass_through_status_change_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto forward_status = event.forward_status;
        auto reverse_status = event.reverse_status;
        fmt::println(
            output_,
            "note over pass_through: pass_through_status_change: zone={} forward_destination={} reverse_destination={} "
            "forward_status={} reverse_status={}",
            zone_id.get_subnet(),
            forward_destination.get_subnet(),
            reverse_destination.get_subnet(),
            static_cast<int>(forward_status),
            static_cast<int>(reverse_status));
        fflush(output_);
        return;
    }

    // Service methods (send/post operations - not RAII, always visible)
    void sequence_diagram_telemetry_service::on_service_send(const rpc::telemetry::service_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = caller_zone_id;

        fmt::println(
            output_,
            "{} -> {} : send obj={} iface={} method={} {}",
            service_alias(zone_id),
            service_alias(destination_zone_id),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_post(const rpc::telemetry::service_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = caller_zone_id;

        fmt::println(
            output_,
            "{} ->> {} : post obj={} iface={} method={} {}",
            service_alias(zone_id),
            service_alias(destination_zone_id),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_object_released(
        const rpc::telemetry::service_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        std::ignore = caller_zone_id;

        fmt::println(
            output_,
            "{} -> {} : object_released obj={} {}",
            service_alias(zone_id),
            service_alias(destination_zone_id),
            object_id.get_val(),
            get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_transport_down(
        const rpc::telemetry::service_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = caller_zone_id;

        fmt::println(
            output_,
            "{} -X {} : transport_down {}",
            service_alias(zone_id),
            service_alias(destination_zone_id),
            get_thread_id());
        fflush(output_);
        return;
    }

    // Service proxy methods (send/post operations - not RAII, always visible)
    void sequence_diagram_telemetry_service::on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "{} -> {} : send obj={} iface={} method={} {}",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_alias(destination_zone_id),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "{} ->> {} : post obj={} iface={} method={} {}",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_alias(destination_zone_id),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val(),
            get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_object_released(
        const rpc::telemetry::service_proxy_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "{} -> {} : object_released obj={} {}",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_alias(destination_zone_id),
            object_id.get_val(),
            get_thread_id());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_service_proxy_transport_down(
        const rpc::telemetry::service_proxy_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        fmt::println(
            output_,
            "{} -X {} : transport_down {}",
            service_proxy_alias(zone_id, destination_zone_id, caller_zone_id),
            service_alias(destination_zone_id),
            get_thread_id());
        fflush(output_);
        return;
    }

    // Transport outbound methods
    void sequence_diagram_telemetry_service::on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_send: adjacent={} dest={} caller={} obj={} iface={} method={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_post: adjacent={} dest={} caller={} obj={} iface={} method={} ",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_outbound_try_cast(
        const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_try_cast: adjacent={} dest={} caller={} obj={} iface={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            interface_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_outbound_add_ref(
        const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_add_ref: adjacent={} dest={} caller={} obj={} "
            "known_direction={} opts={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            requesting_zone_id.get_subnet(),
            static_cast<int>(options));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_outbound_release(
        const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_release: adjacent={} dest={} caller={} obj={} opts={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            static_cast<int>(options));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_outbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_object_released: adjacent={} dest={} caller={} obj={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_outbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        fmt::println(
            output_,
            "note over zone_{}: transport_outbound_transport_down: adjacent={} dest={} caller={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet());
        fflush(output_);
        return;
    }

    // Transport inbound methods (send/post operations - not RAII, always visible)
    void sequence_diagram_telemetry_service::on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_send: adjacent={} dest={} caller={} obj={} iface={} method={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_post: adjacent={} dest={} caller={} obj={} iface={} method={} ",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_inbound_try_cast(
        const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_try_cast: adjacent={} dest={} caller={} obj={} iface={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            interface_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_inbound_add_ref(
        const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_add_ref: adjacent={} dest={} caller={} obj={} "
            "known_direction={} opts={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            requesting_zone_id.get_subnet(),
            static_cast<int>(options));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_inbound_release(
        const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_release: adjacent={} dest={} caller={} obj={} opts={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val(),
            static_cast<int>(options));
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_inbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto destination_zone_id = remote_object_id.as_zone();
        auto object_id = remote_object_id.get_object_id();
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_object_released: adjacent={} dest={} caller={} obj={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet(),
            object_id.get_val());
        fflush(output_);
        return;
    }

    void sequence_diagram_telemetry_service::on_transport_inbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        fmt::println(
            output_,
            "note over zone_{}: transport_inbound_transport_down: adjacent={} dest={} caller={}",
            zone_id.get_subnet(),
            adjacent_zone_id.get_subnet(),
            destination_zone_id.get_subnet(),
            caller_zone_id.get_subnet());
        fflush(output_);
        return;
    }

}
