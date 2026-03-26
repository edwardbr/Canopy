/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/internal/address_utils.h>

namespace rpc
{
    namespace
    {
        // bit layout constants (previously private statics inside zone_address)
        constexpr uint16_t internet_address_bytes = 16u;
        constexpr uint8_t version_bits = 8u;
        constexpr uint8_t address_type_bits = 3u;
        constexpr uint8_t has_port_bits = 1u;
        constexpr uint8_t has_validation_bits = 1u;
        constexpr uint8_t reserved_capability_bits = 3u;
        constexpr uint8_t size_field_bits = 8u;
        constexpr uint8_t port_bits = 16u;
        constexpr uint16_t version_offset_bits = 0u;
        constexpr uint16_t address_type_offset_bits = version_offset_bits + version_bits;
        constexpr uint16_t subnet_size_offset_bits = 16u;
        constexpr uint16_t object_id_size_offset_bits = 24u;
        constexpr uint16_t header_bits = default_values::capability_blob_bytes * 8u;
        constexpr uint8_t address_type_mask = 0x7u;
        constexpr uint8_t has_port_mask = 0x8u;
        constexpr uint8_t has_validation_mask = 0x10u;

        std::vector<uint8_t> capability_bytes_to_vector(const zone_address::capability_bits& caps)
        {
            return std::vector<uint8_t>(caps.begin(), caps.end());
        }

        uint16_t address_bits_for_type(address_type type)
        {
            switch (type)
            {
            case address_type::local:
                return 0;
            case address_type::ipv4:
                return 32;
            case address_type::ipv6:
            case address_type::ipv6_tun:
                return 128;
            }
            return 0;
        }

        [[nodiscard]] rpc::expected<
            void,
            std::string>
        validate_prefix_bits(
            const std::vector<uint8_t>& data,
            uint16_t width,
            const char* field_name)
        {
            auto required_bytes = static_cast<size_t>((width + 7u) / 8u);
            if (data.size() != required_bytes)
                return rpc::unexpected<std::string>(std::string(field_name) + " has the wrong byte width");
            if (width == 0 && !data.empty())
                return rpc::unexpected<std::string>(std::string(field_name) + " must be empty");
            if (width == 0)
                return {};

            auto leading_unused_bits = static_cast<uint8_t>((required_bytes * 8u) - width);
            if (leading_unused_bits == 0)
                return {};
            auto mask = static_cast<uint8_t>(0xffu << (8u - leading_unused_bits));
            if ((data.front() & mask) != 0)
                return rpc::unexpected<std::string>(std::string(field_name) + " does not fit in the declared bit width");
            return {};
        }

        rpc::expected<
            std::vector<uint8_t>,
            std::string>
        build_fixed_width_prefix(
            const std::vector<uint8_t>& data,
            uint16_t width)
        {
            if (auto r = validate_prefix_bits(data, width, "routing_prefix"); !r)
                return rpc::unexpected<std::string>(std::move(r.error()));
            auto byte_width = static_cast<size_t>((width + 7u) / 8u);
            std::vector<uint8_t> result(byte_width, 0);
            for (size_t i = 0; i < data.size(); ++i)
                result[i] = data[i];
            return result;
        }

        rpc::expected<
            std::vector<uint8_t>,
            std::string>
        build_tunnel_host(
            const std::vector<uint8_t>& routing_prefix,
            uint16_t routing_bits,
            uint8_t subnet_size_bits,
            uint64_t subnet,
            uint8_t object_id_size_bits,
            uint64_t object_id)
        {
            auto host = std::vector<uint8_t>(internet_address_bytes, 0);
            auto prefix = build_fixed_width_prefix(routing_prefix, routing_bits);
            if (!prefix)
                return rpc::unexpected<std::string>(std::move(prefix.error()));
            for (size_t i = 0; i < prefix->size(); ++i)
                host[i] = (*prefix)[i];
            if (!set_bits_be(host, routing_bits, subnet_size_bits, subnet))
                return rpc::unexpected<std::string>(std::string("subnet does not fit in subnet_size_bits"));
            if (!set_bits_be(host, static_cast<uint16_t>(routing_bits + subnet_size_bits), object_id_size_bits, object_id))
                return rpc::unexpected<std::string>(std::string("object_id does not fit in object_id_size_bits"));
            return host;
        }

        uint8_t capability_version(const zone_address::capability_bits& caps)
        {
            return static_cast<uint8_t>(get_bits_le(capability_bytes_to_vector(caps), version_offset_bits, version_bits));
        }

        uint8_t capability_header_byte(const zone_address::capability_bits& caps)
        {
            return static_cast<uint8_t>(get_bits_le(
                capability_bytes_to_vector(caps),
                address_type_offset_bits,
                static_cast<uint16_t>(address_type_bits + has_port_bits + has_validation_bits + reserved_capability_bits)));
        }

        address_type capability_address_type(const zone_address::capability_bits& caps)
        {
            return static_cast<address_type>(capability_header_byte(caps) & address_type_mask);
        }

        bool capability_has_port(const zone_address::capability_bits& caps)
        {
            return (capability_header_byte(caps) & has_port_mask) != 0;
        }

        uint8_t capability_subnet_size_bits(const zone_address::capability_bits& caps)
        {
            return static_cast<uint8_t>(
                get_bits_le(capability_bytes_to_vector(caps), subnet_size_offset_bits, size_field_bits));
        }

        uint8_t capability_object_id_size_bits(const zone_address::capability_bits& caps)
        {
            return static_cast<uint8_t>(
                get_bits_le(capability_bytes_to_vector(caps), object_id_size_offset_bits, size_field_bits));
        }

        bool capability_has_validation(const zone_address::capability_bits& caps)
        {
            return (capability_header_byte(caps) & has_validation_mask) != 0;
        }

        zone_address::capability_bits make_capability_bits(
            uint8_t version,
            address_type type,
            bool has_port,
            bool has_validation,
            uint8_t subnet_size_bits,
            uint8_t object_id_size_bits)
        {
            zone_address::capability_bits bits = {};
            auto data = capability_bytes_to_vector(bits);
            [[maybe_unused]] bool ok = set_bits_le(data, version_offset_bits, version_bits, version);
            ok = set_bits_le(
                data,
                address_type_offset_bits,
                static_cast<uint16_t>(address_type_bits + has_port_bits + has_validation_bits + reserved_capability_bits),
                static_cast<uint8_t>(type) | (has_port ? has_port_mask : 0u) | (has_validation ? has_validation_mask : 0u));
            ok = set_bits_le(data, subnet_size_offset_bits, size_field_bits, subnet_size_bits);
            ok = set_bits_le(data, object_id_size_offset_bits, size_field_bits, object_id_size_bits);
            (void)ok;
            for (size_t i = 0; i < bits.size(); ++i)
                bits[i] = data[i];
            return bits;
        }

        [[nodiscard]] rpc::expected<
            void,
            std::string>
        validate_constructor_args(
            const zone_address::capability_bits& caps,
            uint16_t port,
            const std::vector<uint8_t>& routing_prefix,
            uint64_t subnet,
            uint64_t object_id,
            const std::vector<uint8_t>& validation_bits)
        {
            auto version = capability_version(caps);
            auto header_byte = capability_header_byte(caps);
            auto type_code = capability_address_type(caps);
            auto subnet_size_bits = capability_subnet_size_bits(caps);
            auto object_id_size_bits = capability_object_id_size_bits(caps);
            auto has_val = capability_has_validation(caps);
            auto include_port = capability_has_port(caps);

            if (version != default_values::version_3)
                return rpc::unexpected<std::string>("zone_address only supports version 3");

            if ((header_byte & static_cast<uint8_t>(~(address_type_mask | has_port_mask | has_validation_mask))) != 0)
                return rpc::unexpected<std::string>("zone_address capability bits use reserved values");

            if (static_cast<uint8_t>(type_code) > static_cast<uint8_t>(address_type::ipv6_tun))
                return rpc::unexpected<std::string>("zone_address address_type is not supported");

            if (!include_port && port != 0)
                return rpc::unexpected<std::string>("zone_address port provided without has_port capability");

            if (has_val && validation_bits.empty())
                return rpc::unexpected<std::string>("zone_address has_validation set but no validation bytes provided");
            if (!has_val && !validation_bits.empty())
                return rpc::unexpected<std::string>(
                    "zone_address validation bytes provided without has_validation capability");

            if (subnet_size_bits > 64u)
                return rpc::unexpected<std::string>("zone_address subnet_size_bits must be <= 64");
            if (object_id_size_bits > 64u)
                return rpc::unexpected<std::string>("zone_address object_id_size_bits must be <= 64");
            if (subnet_size_bits < 64u && subnet >= (uint64_t(1) << subnet_size_bits) && subnet_size_bits != 0)
                return rpc::unexpected<std::string>("zone_address subnet does not fit in subnet_size_bits");
            if (object_id_size_bits < 64u && object_id >= (uint64_t(1) << object_id_size_bits) && object_id_size_bits != 0)
                return rpc::unexpected<std::string>("zone_address object_id does not fit in object_id_size_bits");

            switch (type_code)
            {
            case address_type::local:
                if (include_port)
                    return rpc::unexpected<std::string>("local zone_address cannot contain a port");
                if (!routing_prefix.empty())
                    return rpc::unexpected<std::string>("local zone_address cannot contain a routing prefix");
                if (!validation_bits.empty())
                    return rpc::unexpected<std::string>("local zone_address cannot contain validation bits");
                break;
            case address_type::ipv4:
                if (auto r = validate_prefix_bits(routing_prefix, 32u, "routing_prefix"); !r)
                    return r;
                break;
            case address_type::ipv6:
                if (auto r = validate_prefix_bits(routing_prefix, 128u, "routing_prefix"); !r)
                    return r;
                break;
            case address_type::ipv6_tun:
            {
                if (subnet_size_bits + object_id_size_bits > 128u)
                    return rpc::unexpected<std::string>("ipv6_tun subnet/object bits exceed 128 bits");
                auto routing_bits = static_cast<uint16_t>(128u - subnet_size_bits - object_id_size_bits);
                if (auto r = validate_prefix_bits(routing_prefix, routing_bits, "routing_prefix"); !r)
                    return r;
                break;
            }
            }
            return {};
        }
    } // namespace

    rpc::expected<
        void,
        std::string>
    zone_address::initialise_blob(
        const capability_bits& caps,
        uint16_t port,
        const std::vector<uint8_t>& routing_prefix,
        uint64_t subnet,
        uint64_t object_id,
        const std::vector<uint8_t>& validation_bits)
    {
        auto type = capability_address_type(caps);
        auto include_port = capability_has_port(caps);
        auto subnet_bits = capability_subnet_size_bits(caps);
        auto object_bits = capability_object_id_size_bits(caps);

        uint32_t total_bits = header_bits;
        if (include_port)
            total_bits += port_bits;
        total_bits += address_bits_for_type(type);
        if (type != address_type::ipv6_tun)
            total_bits += subnet_bits + object_bits;
        total_bits += static_cast<uint32_t>(validation_bits.size() * 8u);

        blob.assign((total_bits + 7u) / 8u, 0);
        auto capability_data = capability_bytes_to_vector(caps);
        for (size_t i = 0; i < caps.size(); ++i)
            blob[i] = capability_data[i];
        [[maybe_unused]] bool ok = true;
        if (include_port)
            ok = set_bits_le(blob, header_bits, port_bits, port);
        (void)ok;

        if (type == address_type::ipv4 || type == address_type::ipv6)
        {
            auto prefix = build_fixed_width_prefix(routing_prefix, address_bits_for_type(type));
            if (!prefix)
                return rpc::unexpected<std::string>(std::move(prefix.error()));
            write_host_bytes(*prefix);
        }
        else if (type == address_type::ipv6_tun)
        {
            auto routing_bits = static_cast<uint16_t>(128u - subnet_bits - object_bits);
            auto host = build_tunnel_host(routing_prefix, routing_bits, subnet_bits, subnet, object_bits, object_id);
            if (!host)
                return rpc::unexpected<std::string>(std::move(host.error()));
            write_host_bytes(*host);
        }

        if (type != address_type::ipv6_tun)
        {
            if (auto r = set_subnet(subnet); !r)
                return r;
            if (auto r = set_object_id(object_id); !r)
                return r;
        }

        if (!validation_bits.empty())
        {
            auto offset = validation_offset_bits();
            for (size_t i = 0; i < validation_bits.size(); ++i)
                blob[(offset / 8u) + i] = validation_bits[i];
        }
        return {};
    }

    uint16_t zone_address::address_offset_bits() const
    {
        return static_cast<uint16_t>(header_bits + (has_port() ? port_bits : 0u));
    }

    uint16_t zone_address::subnet_offset_bits() const
    {
        return static_cast<uint16_t>(address_offset_bits() + address_bits_for_type(get_address_type()));
    }

    uint16_t zone_address::object_offset_bits() const
    {
        return static_cast<uint16_t>(subnet_offset_bits() + get_subnet_size_bits());
    }

    uint16_t zone_address::validation_offset_bits() const
    {
        if (get_address_type() == address_type::ipv6_tun)
            return static_cast<uint16_t>(address_offset_bits() + 128u);
        return static_cast<uint16_t>(object_offset_bits() + get_object_id_size_bits());
    }

    std::vector<uint8_t> zone_address::read_host_bytes() const
    {
        std::vector<uint8_t> host(internet_address_bytes, 0);
        auto type = get_address_type();
        auto address_bits = address_bits_for_type(type);
        if (blob.empty() || address_bits == 0)
            return host;

        auto byte_offset = static_cast<size_t>(address_offset_bits() / 8u);
        auto byte_count = static_cast<size_t>(address_bits / 8u);
        for (size_t i = 0; i < byte_count && i < host.size() && byte_offset + i < blob.size(); ++i)
        {
            host[i] = blob[byte_offset + i];
        }
        return host;
    }

    void zone_address::write_host_bytes(const std::vector<uint8_t>& host)
    {
        auto address_bits = address_bits_for_type(get_address_type());
        if (address_bits == 0)
            return;

        auto byte_offset = static_cast<size_t>(address_offset_bits() / 8u);
        auto byte_count = static_cast<size_t>(address_bits / 8u);
        for (size_t i = 0; i < byte_count && i < host.size() && byte_offset + i < blob.size(); ++i)
        {
            blob[byte_offset + i] = host[i];
        }
    }

    void zone_address::clear_validation_bits()
    {
        if (!has_validation())
            return;
        auto new_size = static_cast<size_t>(validation_offset_bits() / 8u);
        blob.resize(new_size);
        if (blob.size() > 1u)
            blob[1] = static_cast<uint8_t>(blob[1] & ~has_validation_mask);
    }

    rpc::expected<
        zone_address,
        std::string>
    zone_address::create(
        const capability_bits& caps,
        uint16_t port,
        const std::vector<uint8_t>& routing_prefix,
        uint64_t subnet,
        uint64_t object_id,
        const std::vector<uint8_t>& validation_bits)
    {
        if (auto r = validate_constructor_args(caps, port, routing_prefix, subnet, object_id, validation_bits); !r)
            return rpc::unexpected<std::string>(std::move(r.error()));
        zone_address result;
        if (auto r = result.initialise_blob(caps, port, routing_prefix, subnet, object_id, validation_bits); !r)
            return rpc::unexpected<std::string>(std::move(r.error()));
        return result;
    }

    rpc::expected<
        zone_address,
        std::string>
    zone_address::create(const zone_address_args& args)
    {
        auto caps = make_capability_bits(
            args.version, args.type, args.port != 0, !args.validation_bits.empty(), args.subnet_size_bits, args.object_id_size_bits);
        return create(caps, args.port, args.routing_prefix, args.subnet, args.object_id, args.validation_bits);
    }

    uint8_t zone_address::get_version() const
    {
        return blob.empty() ? 0u : static_cast<uint8_t>(get_bits_le(blob, 0, version_bits));
    }

    address_type zone_address::get_address_type() const
    {
        if (blob.empty())
            return address_type::local;
        return static_cast<address_type>(
            get_bits_le(blob, address_type_offset_bits, address_type_bits) & address_type_mask);
    }

    zone_address::capability_bits zone_address::get_capability_bits() const
    {
        capability_bits bits = {};
        for (size_t i = 0; i < bits.size() && i < blob.size(); ++i)
            bits[i] = blob[i];
        return bits;
    }

    bool zone_address::has_port() const
    {
        return capability_has_port(get_capability_bits());
    }

    uint16_t zone_address::get_port() const
    {
        if (!has_port())
            return 0;
        return static_cast<uint16_t>(get_bits_le(blob, header_bits, port_bits));
    }

    uint8_t zone_address::get_subnet_size_bits() const
    {
        return blob.empty() ? 0u : static_cast<uint8_t>(get_bits_le(blob, subnet_size_offset_bits, size_field_bits));
    }

    uint8_t zone_address::get_object_id_size_bits() const
    {
        return blob.empty() ? 0u : static_cast<uint8_t>(get_bits_le(blob, object_id_size_offset_bits, size_field_bits));
    }

    bool zone_address::has_validation() const
    {
        return capability_has_validation(get_capability_bits());
    }

    uint32_t zone_address::get_validation_size_bytes() const
    {
        if (blob.empty())
            return 0u;
        auto offset_bytes = static_cast<size_t>(validation_offset_bits() / 8u);
        if (blob.size() <= offset_bytes)
            return 0u;
        return static_cast<uint32_t>(blob.size() - offset_bytes);
    }

    std::vector<uint8_t> zone_address::get_routing_prefix() const
    {
        auto type = get_address_type();
        if (type == address_type::local)
            return {};

        auto host = read_host_bytes();
        if (type == address_type::ipv4)
        {
            return std::vector<uint8_t>(host.begin(), host.begin() + 4);
        }
        if (type == address_type::ipv6)
        {
            return host;
        }

        auto routing_bits = static_cast<uint16_t>(128u - get_subnet_size_bits() - get_object_id_size_bits());
        auto routing_bytes = static_cast<size_t>((routing_bits + 7u) / 8u);
        std::vector<uint8_t> prefix(routing_bytes, 0);
        for (size_t i = 0; i < routing_bytes; ++i)
        {
            prefix[i] = host[i];
        }
        auto unused_bits = static_cast<uint8_t>((routing_bytes * 8u) - routing_bits);
        if (!prefix.empty() && unused_bits != 0)
        {
            prefix.back() = static_cast<uint8_t>(prefix.back() & (0xffu << unused_bits));
        }
        return prefix;
    }

    uint64_t zone_address::get_subnet() const
    {
        auto subnet_bits = get_subnet_size_bits();
        if (subnet_bits == 0)
            return 0;

        if (get_address_type() == address_type::ipv6_tun)
        {
            auto host = read_host_bytes();
            auto start = static_cast<uint16_t>(128u - get_object_id_size_bits() - subnet_bits);
            return get_bits_be(host, start, subnet_bits);
        }

        return get_bits_le(blob, subnet_offset_bits(), subnet_bits);
    }

    uint64_t zone_address::get_object_id() const
    {
        auto object_bits = get_object_id_size_bits();
        if (object_bits == 0)
            return 0;

        if (get_address_type() == address_type::ipv6_tun)
        {
            auto host = read_host_bytes();
            auto start = static_cast<uint16_t>(128u - object_bits);
            return get_bits_be(host, start, object_bits);
        }

        return get_bits_le(blob, object_offset_bits(), object_bits);
    }

    rpc::expected<
        void,
        std::string>
    zone_address::set_subnet(uint64_t val)
    {
        auto subnet_bits = get_subnet_size_bits();
        if (subnet_bits == 0)
        {
            if (val != 0)
                return rpc::unexpected<std::string>("subnet value is non-zero but subnet_size_bits is 0");
            return {};
        }

        if (subnet_bits < 64u && val >= (uint64_t(1) << subnet_bits))
            return rpc::unexpected<std::string>("subnet value does not fit in subnet_size_bits");

        if (get_address_type() == address_type::ipv6_tun)
        {
            auto host = read_host_bytes();
            auto start = static_cast<uint16_t>(128u - get_object_id_size_bits() - subnet_bits);
            if (!set_bits_be(host, start, subnet_bits, val))
                return rpc::unexpected<std::string>("subnet value does not fit in subnet_size_bits");
            write_host_bytes(host);
            return {};
        }

        if (!set_bits_le(blob, subnet_offset_bits(), subnet_bits, val))
            return rpc::unexpected<std::string>("subnet value does not fit in subnet_size_bits");
        return {};
    }

    rpc::expected<
        void,
        std::string>
    zone_address::set_object_id(uint64_t val)
    {
        auto object_bits = get_object_id_size_bits();
        if (object_bits == 0)
        {
            if (val != 0)
                return rpc::unexpected<std::string>("object_id value is non-zero but object_id_size_bits is 0");
            return {};
        }

        if (object_bits < 64u && val >= (uint64_t(1) << object_bits))
            return rpc::unexpected<std::string>("object_id value does not fit in object_id_size_bits");

        if (get_address_type() == address_type::ipv6_tun)
        {
            auto host = read_host_bytes();
            auto start = static_cast<uint16_t>(128u - object_bits);
            if (!set_bits_be(host, start, object_bits, val))
                return rpc::unexpected<std::string>("object_id value does not fit in object_id_size_bits");
            write_host_bytes(host);
            return {};
        }

        if (!set_bits_le(blob, object_offset_bits(), object_bits, val))
            return rpc::unexpected<std::string>("object_id value does not fit in object_id_size_bits");
        return {};
    }

    zone_address zone_address::zone_only() const
    {
        zone_address copy(*this);
        [[maybe_unused]] auto r = copy.set_object_id(0);
        copy.clear_validation_bits();
        return copy;
    }

    rpc::expected<
        zone_address,
        std::string>
    zone_address::with_object(uint64_t obj) const
    {
        zone_address copy(zone_only());
        if (auto r = copy.set_object_id(obj); !r)
            return rpc::unexpected<std::string>(std::move(r.error()));
        return copy;
    }

    bool zone_address::same_zone(const zone_address& other) const
    {
        if (get_address_type() != other.get_address_type())
            return false;
        if (has_port() != other.has_port())
            return false;
        if (get_port() != other.get_port())
            return false;
        if (get_subnet_size_bits() != other.get_subnet_size_bits())
            return false;
        if (get_object_id_size_bits() != other.get_object_id_size_bits())
            return false;
        if (get_subnet() != other.get_subnet())
            return false;

        auto lhs_host = read_host_bytes();
        auto rhs_host = other.read_host_bytes();
        if (get_address_type() == address_type::ipv6_tun)
        {
            auto object_bits = get_object_id_size_bits();
            for (uint16_t i = 0; i < object_bits; ++i)
            {
                auto bit = static_cast<uint16_t>(127u - i);
                auto byte_index = static_cast<size_t>(bit / 8u);
                auto bit_index = static_cast<uint8_t>(7u - (bit % 8u));
                auto mask = static_cast<uint8_t>(1u << bit_index);
                lhs_host[byte_index] = static_cast<uint8_t>(lhs_host[byte_index] & ~mask);
                rhs_host[byte_index] = static_cast<uint8_t>(rhs_host[byte_index] & ~mask);
            }
        }
        return lhs_host == rhs_host;
    }

    bool zone_address::is_set() const noexcept
    {
        return !get_routing_prefix().empty() || get_subnet() != 0 || get_object_id() != 0 || has_port();
    }

    bool zone_address::operator<(const zone_address& other) const
    {
        return blob < other.blob;
    }
} // namespace rpc
