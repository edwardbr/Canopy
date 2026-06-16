/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_CANONICAL_CRYPTO

#  include <array>
#  include <cmath>
#  include <cstdint>
#  include <cstring>
#  include <limits>
#  include <map>
#  include <optional>
#  include <stdexcept>
#  include <string>
#  include <type_traits>
#  include <utility>
#  include <variant>
#  include <vector>

namespace rpc
{
    template<typename T> class optional;

    inline constexpr size_t canonical_crypto_bytes_per_kib = 1024U;
    inline constexpr size_t canonical_crypto_kib_per_mib = 1024U;
    inline constexpr size_t canonical_crypto_max_field_size_mib = 64U;
    inline constexpr size_t canonical_crypto_max_field_size
        = canonical_crypto_max_field_size_mib * canonical_crypto_kib_per_mib * canonical_crypto_bytes_per_kib;
    inline constexpr int canonical_crypto_bits_per_byte = 8;
    inline constexpr uint64_t canonical_crypto_byte_mask = 0xffU;

    class canonical_crypto_writer
    {
    public:
        explicit canonical_crypto_writer(std::vector<char>& out)
            : out_(out)
        {
        }

        [[nodiscard]] bool ok() const noexcept { return ok_; }

        bool append_u8(uint8_t value)
        {
            if (!can_append(sizeof(value)))
                return fail();
            out_.push_back(static_cast<char>(value));
            return true;
        }

        bool append_u64(uint64_t value)
        {
            if (!can_append(sizeof(value)))
                return fail();
            for (int shift = 56; shift >= 0; shift -= canonical_crypto_bits_per_byte)
                out_.push_back(static_cast<char>((value >> shift) & canonical_crypto_byte_mask));
            return true;
        }

        bool append_bytes(
            const uint8_t* data,
            size_t size)
        {
            if (size > canonical_crypto_max_field_size || !append_u64(static_cast<uint64_t>(size)))
                return false;
            if (size == 0)
                return true;
            if (data == nullptr || !can_append(size))
                return fail();
            out_.insert(out_.end(), reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size);
            return true;
        }

        bool append_chars(
            const char* data,
            size_t size)
        {
            return append_bytes(reinterpret_cast<const uint8_t*>(data), size);
        }

    private:
        [[nodiscard]] bool can_append(size_t size) const noexcept { return size <= out_.max_size() - out_.size(); }

        bool fail() noexcept
        {
            ok_ = false;
            return false;
        }

        std::vector<char>& out_;
        bool ok_{true};
    };

    class canonical_crypto_reader
    {
    public:
        canonical_crypto_reader(
            const void* data,
            size_t size)
            : data_(static_cast<const uint8_t*>(data))
            , size_(size)
        {
            if (size != 0 && data == nullptr)
                ok_ = false;
        }

        explicit canonical_crypto_reader(const std::vector<char>& data)
            : canonical_crypto_reader(
                  data.data(),
                  data.size())
        {
        }

        [[nodiscard]] bool ok() const noexcept { return ok_; }
        [[nodiscard]] bool done() const noexcept { return ok_ && offset_ == size_; }

        bool read_u8(uint8_t& value)
        {
            if (!has_remaining(sizeof(value)))
                return fail();
            value = data_[offset_++];
            return true;
        }

        bool read_u64(uint64_t& value)
        {
            if (!has_remaining(sizeof(value)))
                return fail();
            value = 0;
            for (int shift = 56; shift >= 0; shift -= canonical_crypto_bits_per_byte)
                value |= static_cast<uint64_t>(data_[offset_++]) << shift;
            return true;
        }

        bool read_bytes(std::vector<uint8_t>& value)
        {
            uint64_t size = 0;
            if (!read_u64(size))
                return false;
            if (size > canonical_crypto_max_field_size || size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())
                || !has_remaining(static_cast<size_t>(size)))
            {
                return fail();
            }
            value.assign(
                data_ + static_cast<std::ptrdiff_t>(offset_),
                data_ + static_cast<std::ptrdiff_t>(offset_ + static_cast<size_t>(size)));
            offset_ += static_cast<size_t>(size);
            return true;
        }

        bool read_chars(std::vector<char>& value)
        {
            uint64_t size = 0;
            if (!read_u64(size))
                return false;
            if (size > canonical_crypto_max_field_size || size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())
                || !has_remaining(static_cast<size_t>(size)))
            {
                return fail();
            }
            value.assign(
                reinterpret_cast<const char*>(data_ + static_cast<std::ptrdiff_t>(offset_)),
                reinterpret_cast<const char*>(data_ + static_cast<std::ptrdiff_t>(offset_ + static_cast<size_t>(size))));
            offset_ += static_cast<size_t>(size);
            return true;
        }

    private:
        [[nodiscard]] bool has_remaining(size_t size) const noexcept
        {
            return ok_ && offset_ <= size_ && size <= size_ - offset_;
        }

        bool fail() noexcept
        {
            ok_ = false;
            return false;
        }

        const uint8_t* data_{nullptr};
        size_t size_{0};
        size_t offset_{0};
        bool ok_{true};
    };

    template<typename T, typename = void> struct has_canonical_crypto_write_to : std::false_type
    {
    };

    template<typename T>
    struct has_canonical_crypto_write_to<
        T,
        std::void_t<decltype(std::declval<const T&>().canonical_crypto_write_to(std::declval<canonical_crypto_writer&>()))>>
        : std::true_type
    {
    };

    template<typename T>
    inline constexpr bool has_canonical_crypto_write_to_v = has_canonical_crypto_write_to<T>::value;

    template<typename T, typename = void> struct has_canonical_crypto_read_from : std::false_type
    {
    };

    template<typename T>
    struct has_canonical_crypto_read_from<
        T,
        std::void_t<decltype(std::declval<T&>().canonical_crypto_read_from(std::declval<canonical_crypto_reader&>()))>>
        : std::true_type
    {
    };

    template<typename T>
    inline constexpr bool has_canonical_crypto_read_from_v = has_canonical_crypto_read_from<T>::value;

    template<typename T> struct is_std_vector : std::false_type
    {
    };

    template<typename T, typename Allocator> struct is_std_vector<std::vector<T, Allocator>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

    template<typename T> struct canonical_crypto_is_std_array : std::false_type
    {
    };

    template<typename T, size_t N> struct canonical_crypto_is_std_array<std::array<T, N>> : std::true_type
    {
    };

    template<typename T>
    inline constexpr bool canonical_crypto_is_std_array_v = canonical_crypto_is_std_array<T>::value;

    template<typename T> struct is_std_optional : std::false_type
    {
    };

    template<typename T> struct is_std_optional<std::optional<T>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_std_optional_v = is_std_optional<T>::value;

    template<typename T> struct is_rpc_optional : std::false_type
    {
    };

    template<typename T> struct is_rpc_optional<rpc::optional<T>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_optional_v = is_std_optional_v<T> || is_rpc_optional<T>::value;

    template<typename T> struct is_std_variant : std::false_type
    {
    };

    template<typename... Types> struct is_std_variant<std::variant<Types...>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_std_variant_v = is_std_variant<T>::value;

    template<typename T> struct is_rpc_variant : std::false_type
    {
    };

    template<typename... Types> struct is_rpc_variant<rpc::variant<Types...>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_variant_v = is_std_variant_v<T> || is_rpc_variant<T>::value;

    template<typename T> struct is_std_map : std::false_type
    {
    };

    template<typename Key, typename Value, typename Compare, typename Allocator>
    struct is_std_map<std::map<Key, Value, Compare, Allocator>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_std_map_v = is_std_map<T>::value;

#  if defined(__SIZEOF_INT128__)
    template<typename T> struct is_canonical_crypto_int128 : std::false_type
    {
    };

    template<> struct is_canonical_crypto_int128<__int128> : std::true_type
    {
    };

    template<> struct is_canonical_crypto_int128<unsigned __int128> : std::true_type
    {
    };
#  else
    template<typename T> struct is_canonical_crypto_int128 : std::false_type
    {
    };
#  endif

    template<typename T>
    inline constexpr bool is_canonical_crypto_integral_v = std::is_integral_v<T> || is_canonical_crypto_int128<T>::value;

    template<typename T> struct canonical_crypto_make_unsigned
    {
        using type = std::make_unsigned_t<T>;
    };

#  if defined(__SIZEOF_INT128__)
    template<> struct canonical_crypto_make_unsigned<__int128>
    {
        using type = unsigned __int128;
    };

    template<> struct canonical_crypto_make_unsigned<unsigned __int128>
    {
        using type = unsigned __int128;
    };
#  endif

    template<typename T> using canonical_crypto_make_unsigned_t = typename canonical_crypto_make_unsigned<T>::type;

    template<typename T>
    bool canonical_crypto_write(
        canonical_crypto_writer& writer,
        const T& value);

    template<typename T>
    bool canonical_crypto_read(
        canonical_crypto_reader& reader,
        T& value);

    template<
        typename Variant,
        size_t Index = 0,
        typename = std::enable_if_t<is_variant_v<Variant>>>
    bool canonical_crypto_read_variant_alternative(
        canonical_crypto_reader& reader,
        uint64_t alternative_index,
        Variant& value)
    {
        if constexpr (Index >= rpc::variant_size_v<Variant>)
        {
            (void)reader;
            (void)alternative_index;
            (void)value;
            return false;
        }
        else
        {
            if (alternative_index == Index)
            {
                using alternative_type = rpc::variant_alternative_t<Index, Variant>;
                alternative_type alternative{};
                if (!canonical_crypto_read(reader, alternative))
                    return false;
                value = std::move(alternative);
                return true;
            }
            return canonical_crypto_read_variant_alternative<Variant, Index + 1>(reader, alternative_index, value);
        }
    }

    template<typename T>
    bool canonical_crypto_write_integral(
        canonical_crypto_writer& writer,
        T value)
    {
        using unsigned_type = canonical_crypto_make_unsigned_t<T>;
        auto raw = static_cast<unsigned_type>(value);
        for (int shift = static_cast<int>((sizeof(T) - 1U) * canonical_crypto_bits_per_byte); shift >= 0;
            shift -= canonical_crypto_bits_per_byte)
        {
            if (!writer.append_u8(
                    static_cast<uint8_t>((raw >> shift) & static_cast<unsigned_type>(canonical_crypto_byte_mask))))
                return false;
        }
        return true;
    }

    template<typename T>
    bool canonical_crypto_read_integral(
        canonical_crypto_reader& reader,
        T& value)
    {
        using unsigned_type = canonical_crypto_make_unsigned_t<T>;
        unsigned_type raw = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            uint8_t byte = 0;
            if (!reader.read_u8(byte))
                return false;
            raw = static_cast<unsigned_type>((raw << canonical_crypto_bits_per_byte) | static_cast<unsigned_type>(byte));
        }
        value = static_cast<T>(raw);
        return true;
    }

    template<typename T>
    bool canonical_crypto_write_floating(
        canonical_crypto_writer& writer,
        T value)
    {
        if (!std::isfinite(value))
            return false;
        if constexpr (sizeof(T) == sizeof(uint32_t))
        {
            uint32_t raw = 0;
            std::memcpy(&raw, &value, sizeof(raw));
            return canonical_crypto_write_integral(writer, raw);
        }
        else if constexpr (sizeof(T) == sizeof(uint64_t))
        {
            uint64_t raw = 0;
            std::memcpy(&raw, &value, sizeof(raw));
            return canonical_crypto_write_integral(writer, raw);
        }
        else
        {
            static_assert(
                sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t), "unsupported floating point width");
        }
    }

    template<typename T>
    bool canonical_crypto_read_floating(
        canonical_crypto_reader& reader,
        T& value)
    {
        if constexpr (sizeof(T) == sizeof(uint32_t))
        {
            uint32_t raw = 0;
            if (!canonical_crypto_read_integral(reader, raw))
                return false;
            std::memcpy(&value, &raw, sizeof(value));
            return std::isfinite(value);
        }
        else if constexpr (sizeof(T) == sizeof(uint64_t))
        {
            uint64_t raw = 0;
            if (!canonical_crypto_read_integral(reader, raw))
                return false;
            std::memcpy(&value, &raw, sizeof(value));
            return std::isfinite(value);
        }
        else
        {
            static_assert(
                sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t), "unsupported floating point width");
        }
    }

    template<typename T>
    bool canonical_crypto_write(
        canonical_crypto_writer& writer,
        const T& value)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            return writer.append_u8(value ? 1U : 0U);
        }
        else if constexpr (is_canonical_crypto_integral_v<T>)
        {
            return canonical_crypto_write_integral(writer, value);
        }
        else if constexpr (std::is_enum_v<T>)
        {
            return canonical_crypto_write_integral(writer, static_cast<std::underlying_type_t<T>>(value));
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            return canonical_crypto_write_floating(writer, value);
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            return writer.append_chars(value.data(), value.size());
        }
        else if constexpr (is_std_vector_v<T>)
        {
            using value_type = typename T::value_type;
            if constexpr (std::is_same_v<value_type, char>)
            {
                return writer.append_chars(value.data(), value.size());
            }
            else if constexpr (std::is_same_v<value_type, uint8_t>)
            {
                return writer.append_bytes(value.data(), value.size());
            }
            else
            {
                if (value.size() > canonical_crypto_max_field_size
                    || !writer.append_u64(static_cast<uint64_t>(value.size())))
                    return false;
                for (const auto& entry : value)
                {
                    if (!canonical_crypto_write(writer, entry))
                        return false;
                }
                return true;
            }
        }
        else if constexpr (canonical_crypto_is_std_array_v<T>)
        {
            for (const auto& entry : value)
            {
                if (!canonical_crypto_write(writer, entry))
                    return false;
            }
            return true;
        }
        else if constexpr (is_optional_v<T>)
        {
            if (!writer.append_u8(value.has_value() ? 1U : 0U))
                return false;
            if (!value)
                return true;
            return canonical_crypto_write(writer, *value);
        }
        else if constexpr (is_variant_v<T>)
        {
            if (value.valueless_by_exception() || !writer.append_u64(static_cast<uint64_t>(value.index())))
                return false;
            return rpc::visit([&](const auto& alternative) { return canonical_crypto_write(writer, alternative); }, value);
        }
        else if constexpr (is_std_map_v<T>)
        {
            if (value.size() > canonical_crypto_max_field_size || !writer.append_u64(static_cast<uint64_t>(value.size())))
                return false;
            for (const auto& entry : value)
            {
                if (!canonical_crypto_write(writer, entry.first) || !canonical_crypto_write(writer, entry.second))
                    return false;
            }
            return true;
        }
        else if constexpr (has_canonical_crypto_write_to_v<T>)
        {
            return value.canonical_crypto_write_to(writer) && writer.ok();
        }
        else
        {
            static_assert(has_canonical_crypto_write_to_v<T>, "type does not support canonical_crypto serialization");
        }
    }

    template<typename T>
    bool canonical_crypto_read(
        canonical_crypto_reader& reader,
        T& value)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            uint8_t raw = 0;
            if (!reader.read_u8(raw) || raw > 1U)
                return false;
            value = raw != 0U;
            return true;
        }
        else if constexpr (is_canonical_crypto_integral_v<T>)
        {
            return canonical_crypto_read_integral(reader, value);
        }
        else if constexpr (std::is_enum_v<T>)
        {
            std::underlying_type_t<T> raw = 0;
            if (!canonical_crypto_read_integral(reader, raw))
                return false;
            value = static_cast<T>(raw);
            return true;
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            return canonical_crypto_read_floating(reader, value);
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            std::vector<char> raw;
            if (!reader.read_chars(raw))
                return false;
            value.assign(raw.begin(), raw.end());
            return true;
        }
        else if constexpr (is_std_vector_v<T>)
        {
            using value_type = typename T::value_type;
            if constexpr (std::is_same_v<value_type, char>)
            {
                return reader.read_chars(value);
            }
            else if constexpr (std::is_same_v<value_type, uint8_t>)
            {
                return reader.read_bytes(value);
            }
            else
            {
                uint64_t size = 0;
                if (!reader.read_u64(size) || size > canonical_crypto_max_field_size
                    || size > static_cast<uint64_t>(value.max_size()))
                {
                    return false;
                }
                value.clear();
                value.reserve(static_cast<size_t>(size));
                for (uint64_t i = 0; i < size; ++i)
                {
                    value_type entry{};
                    if (!canonical_crypto_read(reader, entry))
                        return false;
                    value.push_back(std::move(entry));
                }
                return true;
            }
        }
        else if constexpr (canonical_crypto_is_std_array_v<T>)
        {
            for (auto& entry : value)
            {
                if (!canonical_crypto_read(reader, entry))
                    return false;
            }
            return true;
        }
        else if constexpr (is_optional_v<T>)
        {
            uint8_t present = 0;
            if (!reader.read_u8(present) || present > 1U)
                return false;
            if (present == 0U)
            {
                value.reset();
                return true;
            }
            typename T::value_type entry{};
            if (!canonical_crypto_read(reader, entry))
                return false;
            value = std::move(entry);
            return true;
        }
        else if constexpr (is_variant_v<T>)
        {
            uint64_t alternative_index = 0;
            if (!reader.read_u64(alternative_index) || alternative_index >= rpc::variant_size_v<T>)
                return false;
            return canonical_crypto_read_variant_alternative(reader, alternative_index, value);
        }
        else if constexpr (is_std_map_v<T>)
        {
            uint64_t size = 0;
            if (!reader.read_u64(size) || size > canonical_crypto_max_field_size
                || size > static_cast<uint64_t>(value.max_size()))
                return false;
            value.clear();
            for (uint64_t i = 0; i < size; ++i)
            {
                typename T::key_type key{};
                typename T::mapped_type mapped{};
                if (!canonical_crypto_read(reader, key) || !canonical_crypto_read(reader, mapped))
                    return false;
                auto inserted = value.emplace(std::move(key), std::move(mapped));
                if (!inserted.second)
                    return false;
            }
            return true;
        }
        else if constexpr (has_canonical_crypto_read_from_v<T>)
        {
            return value.canonical_crypto_read_from(reader);
        }
        else
        {
            static_assert(has_canonical_crypto_read_from_v<T>, "type does not support canonical_crypto deserialization");
        }
    }
}

#endif
