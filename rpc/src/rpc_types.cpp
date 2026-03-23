/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <stdexcept>

#include <rpc/rpc.h>

namespace rpc
{
    namespace
    {
        std::vector<uint8_t> capability_bytes_to_vector(const std::array<uint8_t, zone_address::capability_blob_bytes>& capability_bits)
        {
            return std::vector<uint8_t>(capability_bits.begin(), capability_bits.end());
        }
    } // namespace

    uint64_t zone_address::get_bits_le(const std::vector<uint8_t>& data, uint16_t offset, uint16_t width)
    {
        if (width == 0)
            return 0;

        if (width > 64u)
            width = 64u;

        uint64_t value = 0;
        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = offset + i;
            auto byte_index = static_cast<size_t>(bit / 8u);
            if (byte_index >= data.size())
                break;

            auto mask = static_cast<uint8_t>(1u << (bit % 8u));
            if ((data[byte_index] & mask) != 0)
                value |= (uint64_t(1) << i);
        }
        return value;
    }

    bool zone_address::set_bits_le(std::vector<uint8_t>& data, uint16_t offset, uint16_t width, uint64_t value)
    {
        if (width == 0)
            return value == 0;

        if (width < 64u && value >= (uint64_t(1) << width))
            return false;

        auto required_bits = static_cast<uint32_t>(offset) + static_cast<uint32_t>(width);
        auto required_bytes = static_cast<size_t>((required_bits + 7u) / 8u);
        if (data.size() < required_bytes)
            data.resize(required_bytes, 0);

        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = offset + i;
            auto& byte = data[bit / 8u];
            auto mask = static_cast<uint8_t>(1u << (bit % 8u));
            if (((value >> i) & 1u) != 0)
                byte = static_cast<uint8_t>(byte | mask);
            else
                byte = static_cast<uint8_t>(byte & ~mask);
        }
        return true;
    }

    uint64_t zone_address::get_bits_be(const std::vector<uint8_t>& data, uint16_t offset, uint16_t width)
    {
        if (width == 0)
            return 0;

        if (width > 64u)
            width = 64u;

        uint64_t value = 0;
        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = offset + i;
            auto byte_index = static_cast<size_t>(bit / 8u);
            auto bit_index = static_cast<uint8_t>(7u - (bit % 8u));
            value <<= 1u;
            value |= static_cast<uint64_t>((data[byte_index] >> bit_index) & 1u);
        }
        return value;
    }

    bool zone_address::set_bits_be(std::vector<uint8_t>& data, uint16_t offset, uint16_t width, uint64_t value)
    {
        if (width == 0)
            return value == 0;

        if (width < 64u && value >= (uint64_t(1) << width))
            return false;

        for (uint16_t i = 0; i < width; ++i)
        {
            auto bit = static_cast<uint16_t>(offset + width - 1u - i);
            auto byte_index = static_cast<size_t>(bit / 8u);
            auto bit_index = static_cast<uint8_t>(7u - (bit % 8u));
            auto mask = static_cast<uint8_t>(1u << bit_index);
            if (((value >> i) & 1u) != 0)
                data[byte_index] = static_cast<uint8_t>(data[byte_index] | mask);
            else
                data[byte_index] = static_cast<uint8_t>(data[byte_index] & ~mask);
        }
        return true;
    }

    uint16_t zone_address::address_bits_for_type(address_type type)
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

    void zone_address::validate_prefix_bits(const std::vector<uint8_t>& data, uint16_t width, const char* field_name)
    {
        auto required_bytes = static_cast<size_t>((width + 7u) / 8u);
        if (data.size() != required_bytes)
            throw std::invalid_argument(std::string(field_name) + " has the wrong byte width");
        if (width == 0 && !data.empty())
            throw std::invalid_argument(std::string(field_name) + " must be empty");
        if (width == 0)
            return;

        auto leading_unused_bits = static_cast<uint8_t>((required_bytes * 8u) - width);
        if (leading_unused_bits == 0)
            return;
        auto mask = static_cast<uint8_t>(0xffu << (8u - leading_unused_bits));
        if ((data.front() & mask) != 0)
            throw std::invalid_argument(std::string(field_name) + " does not fit in the declared bit width");
    }

    std::vector<uint8_t> zone_address::build_fixed_width_prefix(const std::vector<uint8_t>& data, uint16_t width)
    {
        validate_prefix_bits(data, width, "routing_prefix");
        auto byte_width = static_cast<size_t>((width + 7u) / 8u);
        std::vector<uint8_t> result(byte_width, 0);
        for (size_t i = 0; i < data.size(); ++i)
        {
            result[i] = data[i];
        }
        return result;
    }

    std::vector<uint8_t> zone_address::build_tunnel_host(
        const std::vector<uint8_t>& routing_prefix, uint16_t routing_bits, uint8_t subnet_size_bits, uint64_t subnet, uint8_t object_id_size_bits,
        uint64_t object_id)
    {
        auto host = std::vector<uint8_t>(internet_address_bytes, 0);
        auto prefix = build_fixed_width_prefix(routing_prefix, routing_bits);
        for (size_t i = 0; i < prefix.size(); ++i)
        {
            host[i] = prefix[i];
        }

        if (!set_bits_be(host, routing_bits, subnet_size_bits, subnet))
            throw std::invalid_argument("subnet does not fit in subnet_size_bits");
        if (!set_bits_be(host, static_cast<uint16_t>(routing_bits + subnet_size_bits), object_id_size_bits, object_id))
            throw std::invalid_argument("object_id does not fit in object_id_size_bits");
        return host;
    }

    uint8_t zone_address::capability_version(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return static_cast<uint8_t>(get_bits_le(capability_bytes_to_vector(capability_bits), version_offset_bits, version_bits));
    }

    uint8_t zone_address::capability_header_byte(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return static_cast<uint8_t>(get_bits_le(capability_bytes_to_vector(capability_bits), address_type_offset_bits,
            static_cast<uint16_t>(address_type_bits + has_port_bits + reserved_capability_bits)));
    }

    zone_address::address_type zone_address::capability_address_type(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return static_cast<address_type>(capability_header_byte(capability_bits) & address_type_mask);
    }

    bool zone_address::capability_has_port(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return (capability_header_byte(capability_bits) & has_port_mask) != 0;
    }

    uint8_t zone_address::capability_subnet_size_bits(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return static_cast<uint8_t>(get_bits_le(capability_bytes_to_vector(capability_bits), subnet_size_offset_bits, size_field_bits));
    }

    uint8_t zone_address::capability_object_id_size_bits(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return static_cast<uint8_t>(get_bits_le(capability_bytes_to_vector(capability_bits), object_id_size_offset_bits, size_field_bits));
    }

    uint16_t zone_address::capability_validation_size_bits(const std::array<uint8_t, capability_blob_bytes>& capability_bits)
    {
        return static_cast<uint16_t>(get_bits_le(capability_bytes_to_vector(capability_bits), validation_size_offset_bits, validation_size_field_bits));
    }

    std::array<uint8_t, zone_address::capability_blob_bytes> zone_address::make_capability_bits(
        uint8_t version, address_type type, bool has_port, uint8_t subnet_size_bits, uint8_t object_id_size_bits, uint16_t validation_size_bits)
    {
        std::array<uint8_t, capability_blob_bytes> bits = {};
        auto data = capability_bytes_to_vector(bits);
        [[maybe_unused]] bool ok = set_bits_le(data, version_offset_bits, version_bits, version);
        ok = set_bits_le(data, address_type_offset_bits,
            static_cast<uint16_t>(address_type_bits + has_port_bits + reserved_capability_bits),
            static_cast<uint8_t>(type) | (has_port ? has_port_mask : 0u));
        ok = set_bits_le(data, subnet_size_offset_bits, size_field_bits, subnet_size_bits);
        ok = set_bits_le(data, object_id_size_offset_bits, size_field_bits, object_id_size_bits);
        ok = set_bits_le(data, validation_size_offset_bits, validation_size_field_bits, validation_size_bits);
        (void)ok;
        for (size_t i = 0; i < bits.size(); ++i)
            bits[i] = data[i];
        return bits;
    }

    void zone_address::validate_constructor_args(const std::array<uint8_t, capability_blob_bytes>& capability_bits, uint16_t port,
        const std::vector<uint8_t>& routing_prefix, uint64_t subnet, uint64_t object_id, const std::vector<uint8_t>& validation_bits)
    {
        auto version = capability_version(capability_bits);
        auto header_byte = capability_header_byte(capability_bits);
        auto type_code = capability_address_type(capability_bits);
        auto subnet_size_bits = capability_subnet_size_bits(capability_bits);
        auto object_id_size_bits = capability_object_id_size_bits(capability_bits);
        auto validation_size_bits = capability_validation_size_bits(capability_bits);
        auto include_port = capability_has_port(capability_bits);

        if (version != version_3)
            throw std::invalid_argument("zone_address only supports version 3");

        if ((header_byte & static_cast<uint8_t>(~(address_type_mask | has_port_mask))) != 0)
            throw std::invalid_argument("zone_address capability bits use reserved values");

        if (static_cast<uint8_t>(type_code) > static_cast<uint8_t>(address_type::ipv6_tun))
            throw std::invalid_argument("zone_address address_type is not supported");

        if (!include_port && port != 0)
            throw std::invalid_argument("zone_address port provided without has_port capability");

        if (validation_bits.size() > (UINT16_MAX / 8u))
            throw std::invalid_argument("zone_address validation_bits exceed uint16 bit count");
        if (validation_size_bits != validation_bits.size() * 8u)
            throw std::invalid_argument("zone_address validation_bits size does not match capability bits");

        if (subnet_size_bits > 64u)
            throw std::invalid_argument("zone_address subnet_size_bits must be <= 64");
        if (object_id_size_bits > 64u)
            throw std::invalid_argument("zone_address object_id_size_bits must be <= 64");
        if (subnet_size_bits < 64u && subnet >= (uint64_t(1) << subnet_size_bits) && subnet_size_bits != 0)
            throw std::invalid_argument("zone_address subnet does not fit in subnet_size_bits");
        if (object_id_size_bits < 64u && object_id >= (uint64_t(1) << object_id_size_bits) && object_id_size_bits != 0)
            throw std::invalid_argument("zone_address object_id does not fit in object_id_size_bits");

        switch (type_code)
        {
        case address_type::local:
            if (include_port)
                throw std::invalid_argument("local zone_address cannot contain a port");
            if (!routing_prefix.empty())
                throw std::invalid_argument("local zone_address cannot contain a routing prefix");
            if (!validation_bits.empty())
                throw std::invalid_argument("local zone_address cannot contain validation bits");
            break;
        case address_type::ipv4:
            validate_prefix_bits(routing_prefix, 32u, "routing_prefix");
            break;
        case address_type::ipv6:
            validate_prefix_bits(routing_prefix, 128u, "routing_prefix");
            break;
        case address_type::ipv6_tun:
        {
            auto routing_bits = static_cast<uint16_t>(128u - subnet_size_bits - object_id_size_bits);
            if (subnet_size_bits + object_id_size_bits > 128u)
                throw std::invalid_argument("ipv6_tun subnet/object bits exceed 128 bits");
            validate_prefix_bits(routing_prefix, routing_bits, "routing_prefix");
            break;
        }
        }
    }

    void zone_address::initialise_blob(const std::array<uint8_t, capability_blob_bytes>& capability_bits, uint16_t port,
        const std::vector<uint8_t>& routing_prefix, uint64_t subnet, uint64_t object_id, const std::vector<uint8_t>& validation_bits)
    {
        auto type = capability_address_type(capability_bits);
        auto include_port = capability_has_port(capability_bits);
        auto subnet_bits = capability_subnet_size_bits(capability_bits);
        auto object_bits = capability_object_id_size_bits(capability_bits);
        auto validation_size_bits = capability_validation_size_bits(capability_bits);
        if (type == address_type::local)
        {
            include_port = false;
            validation_size_bits = 0;
        }

        uint32_t total_bits = header_bits;
        if (include_port)
            total_bits += port_bits;
        total_bits += address_bits_for_type(type);
        if (type == address_type::ipv6_tun)
            total_bits += validation_size_bits;
        else
            total_bits += subnet_bits + object_bits + validation_size_bits;

        blob.assign((total_bits + 7u) / 8u, 0);
        auto capability_data = capability_bytes_to_vector(capability_bits);
        for (size_t i = 0; i < capability_bits.size(); ++i)
            blob[i] = capability_data[i];
        [[maybe_unused]] bool ok = true;
        if (include_port)
            ok = set_bits_le(blob, header_bits, port_bits, port);
        (void)ok;

        if (type == address_type::ipv4 || type == address_type::ipv6)
        {
            write_host_bytes(build_fixed_width_prefix(routing_prefix, address_bits_for_type(type)));
        }
        else if (type == address_type::ipv6_tun)
        {
            auto routing_bits = static_cast<uint16_t>(128u - subnet_bits - object_bits);
            write_host_bytes(build_tunnel_host(routing_prefix, routing_bits, subnet_bits, subnet, object_bits, object_id));
        }

        if (type != address_type::ipv6_tun)
        {
            [[maybe_unused]] bool subnet_ok = set_subnet(subnet);
            [[maybe_unused]] bool object_ok = set_object_id(object_id);
            (void)subnet_ok;
            (void)object_ok;
        }

        if (!validation_bits.empty())
        {
            auto offset = validation_offset_bits();
            for (size_t i = 0; i < validation_bits.size(); ++i)
            {
                blob[(offset / 8u) + i] = validation_bits[i];
            }
        }
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
        auto validation_bits = get_validation_size_bits();
        if (validation_bits == 0)
            return;

        [[maybe_unused]] bool ok = set_bits_le(blob, validation_offset_bits(), validation_bits, 0);
        (void)ok;
    }

    zone_address::zone_address(const construction_args& args)
        : zone_address(make_capability_bits(args.version, args.type, args.port != 0, args.subnet_size_bits, args.object_id_size_bits,
              static_cast<uint16_t>(args.validation_bits.size() * 8u)),
              args.port, args.routing_prefix, args.subnet, args.object_id, args.validation_bits)
    {
    }

    zone_address::zone_address(const std::array<uint8_t, capability_blob_bytes>& capability_bits, uint16_t port,
        const std::vector<uint8_t>& routing_prefix, uint64_t subnet, uint64_t object_id, const std::vector<uint8_t>& validation_bits)
    {
        validate_constructor_args(capability_bits, port, routing_prefix, subnet, object_id, validation_bits);
        initialise_blob(capability_bits, port, routing_prefix, subnet, object_id, validation_bits);
    }

    uint8_t zone_address::get_version() const
    {
        return blob.empty() ? 0u : static_cast<uint8_t>(get_bits_le(blob, 0, version_bits));
    }

    zone_address::address_type zone_address::get_address_type() const
    {
        if (blob.empty())
            return address_type::local;
        return static_cast<address_type>(get_bits_le(blob, address_type_offset_bits, address_type_bits) & address_type_mask);
    }

    std::array<uint8_t, zone_address::capability_blob_bytes> zone_address::get_capability_bits() const
    {
        std::array<uint8_t, capability_blob_bytes> bits = {};
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

    uint16_t zone_address::get_validation_size_bits() const
    {
        return blob.empty() ? 0u : static_cast<uint16_t>(get_bits_le(blob, validation_size_offset_bits, validation_size_field_bits));
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

    bool zone_address::set_subnet(uint64_t val)
    {
        auto subnet_bits = get_subnet_size_bits();
        if (subnet_bits == 0)
            return val == 0;

        if (subnet_bits < 64u && val >= (uint64_t(1) << subnet_bits))
            return false;

        if (get_address_type() == address_type::ipv6_tun)
        {
            auto host = read_host_bytes();
            auto start = static_cast<uint16_t>(128u - get_object_id_size_bits() - subnet_bits);
            if (!set_bits_be(host, start, subnet_bits, val))
                return false;
            write_host_bytes(host);
            return true;
        }

        return set_bits_le(blob, subnet_offset_bits(), subnet_bits, val);
    }

    bool zone_address::set_object_id(uint64_t val)
    {
        auto object_bits = get_object_id_size_bits();
        if (object_bits == 0)
            return val == 0;

        if (object_bits < 64u && val >= (uint64_t(1) << object_bits))
            return false;

        if (get_address_type() == address_type::ipv6_tun)
        {
            auto host = read_host_bytes();
            auto start = static_cast<uint16_t>(128u - object_bits);
            if (!set_bits_be(host, start, object_bits, val))
                return false;
            write_host_bytes(host);
            return true;
        }

        return set_bits_le(blob, object_offset_bits(), object_bits, val);
    }

    zone_address zone_address::zone_only() const
    {
        zone_address copy(*this);
        [[maybe_unused]] bool ok = copy.set_object_id(0);
        copy.clear_validation_bits();
        (void)ok;
        return copy;
    }

    zone_address zone_address::with_object(uint64_t obj) const
    {
        zone_address copy(*this);
        [[maybe_unused]] bool ok = copy.set_object_id(obj);
        (void)ok;
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
