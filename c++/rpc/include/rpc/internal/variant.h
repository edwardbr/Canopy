/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <yas/detail/config/config.hpp>
#include <yas/detail/io/serialization_exceptions.hpp>
#include <yas/detail/tools/json_tools.hpp>
#include <yas/detail/type_traits/serializer.hpp>
#include <yas/detail/type_traits/type_traits.hpp>

namespace rpc
{
    template<typename T> class id;

    template<typename... Types> class variant
    {
    public:
        using storage_type = std::variant<Types...>;

        variant() = default;

        variant(const storage_type& value)
            : value_(value)
        {
        }

        variant(storage_type&& value)
            : value_(std::move(value))
        {
        }

        template<
            typename T,
            typename = std::enable_if_t<
                !std::is_same_v<
                    std::decay_t<T>,
                    variant>
                && !std::is_same_v<
                    std::decay_t<T>,
                    storage_type>>>
        variant(T&& value)
            : value_(std::forward<T>(value))
        {
        }

        variant& operator=(const storage_type& value)
        {
            value_ = value;
            return *this;
        }

        variant& operator=(storage_type&& value)
        {
            value_ = std::move(value);
            return *this;
        }

        template<
            typename T,
            typename = std::enable_if_t<
                !std::is_same_v<
                    std::decay_t<T>,
                    variant>
                && !std::is_same_v<
                    std::decay_t<T>,
                    storage_type>>>
        variant& operator=(T&& value)
        {
            value_ = std::forward<T>(value);
            return *this;
        }

        [[nodiscard]] constexpr std::size_t index() const noexcept { return value_.index(); }
        [[nodiscard]] constexpr bool valueless_by_exception() const noexcept { return value_.valueless_by_exception(); }

        [[nodiscard]] constexpr const storage_type& as_std_variant() const& noexcept { return value_; }
        [[nodiscard]] constexpr storage_type& as_std_variant() & noexcept { return value_; }
        [[nodiscard]] constexpr const storage_type&& as_std_variant() const&& noexcept { return std::move(value_); }
        [[nodiscard]] constexpr storage_type&& as_std_variant() && noexcept { return std::move(value_); }

    private:
        storage_type value_;
    };

    template<typename... Types>
    [[nodiscard]] constexpr bool operator==(
        const variant<Types...>& lhs,
        const variant<Types...>& rhs)
    {
        return lhs.as_std_variant() == rhs.as_std_variant();
    }

    template<typename... Types>
    [[nodiscard]] constexpr bool operator!=(
        const variant<Types...>& lhs,
        const variant<Types...>& rhs)
    {
        return !(lhs == rhs);
    }

    template<typename T> struct variant_size : std::variant_size<T>
    {
    };

    template<typename... Types>
    struct variant_size<variant<Types...>> : std::integral_constant<std::size_t, sizeof...(Types)>
    {
    };

    template<typename T> inline constexpr std::size_t variant_size_v = variant_size<T>::value;

    template<std::size_t Index, typename T> struct variant_alternative : std::variant_alternative<Index, T>
    {
    };

    template<std::size_t Index, typename... Types>
    struct variant_alternative<Index, variant<Types...>>
        : std::variant_alternative<Index, typename variant<Types...>::storage_type>
    {
    };

    template<std::size_t Index, typename T> using variant_alternative_t = typename variant_alternative<Index, T>::type;

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(variant<Types...>& value)
    {
        return std::get<Index>(value.as_std_variant());
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const variant<Types...>& value)
    {
        return std::get<Index>(value.as_std_variant());
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(variant<Types...>&& value)
    {
        return std::get<Index>(std::move(value).as_std_variant());
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const variant<Types...>&& value)
    {
        return std::get<Index>(std::move(value).as_std_variant());
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(variant<Types...>& value)
    {
        return std::get<T>(value.as_std_variant());
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const variant<Types...>& value)
    {
        return std::get<T>(value.as_std_variant());
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(variant<Types...>&& value)
    {
        return std::get<T>(std::move(value).as_std_variant());
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const variant<Types...>&& value)
    {
        return std::get<T>(std::move(value).as_std_variant());
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(std::variant<Types...>& value)
    {
        return std::get<Index>(value);
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const std::variant<Types...>& value)
    {
        return std::get<Index>(value);
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(std::variant<Types...>&& value)
    {
        return std::get<Index>(std::move(value));
    }

    template<
        std::size_t Index,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const std::variant<Types...>&& value)
    {
        return std::get<Index>(std::move(value));
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(std::variant<Types...>& value)
    {
        return std::get<T>(value);
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const std::variant<Types...>& value)
    {
        return std::get<T>(value);
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(std::variant<Types...>&& value)
    {
        return std::get<T>(std::move(value));
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr decltype(auto) get(const std::variant<Types...>&& value)
    {
        return std::get<T>(std::move(value));
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr auto get_if(variant<Types...>* value) noexcept -> T*
    {
        return value ? std::get_if<T>(&value->as_std_variant()) : nullptr;
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr auto get_if(const variant<Types...>* value) noexcept -> const T*
    {
        return value ? std::get_if<T>(&value->as_std_variant()) : nullptr;
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr auto get_if(std::variant<Types...>* value) noexcept -> T*
    {
        return std::get_if<T>(value);
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr auto get_if(const std::variant<Types...>* value) noexcept -> const T*
    {
        return std::get_if<T>(value);
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr bool holds_alternative(const variant<Types...>& value) noexcept
    {
        return std::holds_alternative<T>(value.as_std_variant());
    }

    template<
        typename T,
        typename... Types>
    [[nodiscard]] constexpr bool holds_alternative(const std::variant<Types...>& value) noexcept
    {
        return std::holds_alternative<T>(value);
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        variant<Types...>& value)
    {
        return std::visit(std::forward<Visitor>(visitor), value.as_std_variant());
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        const variant<Types...>& value)
    {
        return std::visit(std::forward<Visitor>(visitor), value.as_std_variant());
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        variant<Types...>&& value)
    {
        return std::visit(std::forward<Visitor>(visitor), std::move(value).as_std_variant());
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        const variant<Types...>&& value)
    {
        return std::visit(std::forward<Visitor>(visitor), std::move(value).as_std_variant());
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        std::variant<Types...>& value)
    {
        return std::visit(std::forward<Visitor>(visitor), value);
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        const std::variant<Types...>& value)
    {
        return std::visit(std::forward<Visitor>(visitor), value);
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        std::variant<Types...>&& value)
    {
        return std::visit(std::forward<Visitor>(visitor), std::move(value));
    }

    template<
        typename Visitor,
        typename... Types>
    constexpr decltype(auto) visit(
        Visitor&& visitor,
        const std::variant<Types...>&& value)
    {
        return std::visit(std::forward<Visitor>(visitor), std::move(value));
    }

    template<typename... Types> class id<variant<Types...>>
    {
    public:
        static constexpr uint64_t get(uint64_t rpc_version)
        {
            auto value = 0x8F0B7214A9D63C5Dull;
            ((value = ((value ^ id<Types>::get(rpc_version)) << 7U) | ((value ^ id<Types>::get(rpc_version)) >> 57U)), ...);
            return value;
        }
    };

    // JSON-tagged-union name for each alternative of an rpc::variant. The
    // variant serializers use these tags as the single-key in JSON objects
    // ({"<tag>": value}). The same name table is consumed by the DOM-based
    // converters in <json/convert.h> and by the generated JSON schema, so all
    // three layers agree on the wire shape. Primitive specializations are
    // provided here; IDL-defined struct and enum types get their tag
    // specializations emitted by the generator alongside the IDL header.
    template<typename T> struct variant_alternative_tag;

#define CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(Type, Name)                                                             \
    template<> struct variant_alternative_tag<Type>                                                                    \
    {                                                                                                                  \
        static constexpr const char* value = Name;                                                                     \
    }

    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        bool,
        "bool");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::int8_t,
        "int8");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::int16_t,
        "int16");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::int32_t,
        "int32");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::int64_t,
        "int64");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::uint8_t,
        "uint8");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::uint16_t,
        "uint16");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::uint32_t,
        "uint32");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::uint64_t,
        "uint64");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        float,
        "float");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        double,
        "double");
    CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG(
        std::string,
        "string");

#undef CANOPY_DECLARE_VARIANT_ALTERNATIVE_TAG
} // namespace rpc

namespace yas::detail
{
    namespace rpc_variant_detail
    {
        // Binary path: byte index dispatch. Unchanged from the previous wire
        // format because there is no readable text for byte-index variants.
        template<
            std::size_t F,
            typename Archive,
            typename Variant,
            std::size_t Index = 0>
        Archive& save_alternative(
            Archive& ar,
            const Variant& value)
        {
            if constexpr (Index >= rpc::variant_size_v<Variant>)
            {
                (void)value;
                throw std::runtime_error("Unknown rpc::variant alternative");
            }
            else
            {
                if (value.index() == Index)
                {
                    ar& rpc::get<Index>(value);
                    return ar;
                }
                return save_alternative<F, Archive, Variant, Index + 1U>(ar, value);
            }
        }

        template<
            std::size_t F,
            typename Archive,
            typename Variant,
            std::size_t Index = 0>
        Archive& load_alternative(
            Archive& ar,
            std::size_t alternative_index,
            Variant& value)
        {
            if constexpr (Index >= rpc::variant_size_v<Variant>)
            {
                (void)alternative_index;
                (void)value;
                throw std::runtime_error("Unknown rpc::variant alternative");
            }
            else
            {
                if (alternative_index == Index)
                {
                    using alternative_type = rpc::variant_alternative_t<Index, Variant>;
                    alternative_type alternative{};
                    ar & alternative;
                    value = std::move(alternative);
                    return ar;
                }
                return load_alternative<F, Archive, Variant, Index + 1U>(ar, alternative_index, value);
            }
        }

        // JSON path: tag-keyed dispatch using rpc::variant_alternative_tag<T>
        // so the wire shape stays in sync with the schema and the DOM-based
        // converter in <json/convert.h>.
        template<
            std::size_t F,
            typename Archive,
            typename Variant,
            std::size_t Index = 0>
        Archive& save_alternative_tagged(
            Archive& ar,
            const Variant& value)
        {
            if constexpr (Index >= rpc::variant_size_v<Variant>)
            {
                (void)value;
                throw std::runtime_error("Unknown rpc::variant alternative");
            }
            else
            {
                if (value.index() == Index)
                {
                    using alternative_type = rpc::variant_alternative_t<Index, Variant>;
                    const char* tag = rpc::variant_alternative_tag<alternative_type>::value;
                    ar.write("\"", 1);
                    const std::size_t tag_length = std::char_traits<char>::length(tag);
                    ar.write(tag, tag_length);
                    ar.write("\":", 2);
                    ar& rpc::get<Index>(value);
                    return ar;
                }
                return save_alternative_tagged<F, Archive, Variant, Index + 1U>(ar, value);
            }
        }

        template<
            std::size_t F,
            typename Archive,
            typename Variant,
            std::size_t Index = 0>
        Archive& load_alternative_by_tag(
            Archive& ar,
            const char* key,
            Variant& value)
        {
            if constexpr (Index >= rpc::variant_size_v<Variant>)
            {
                (void)ar;
                (void)key;
                (void)value;
                __YAS_THROW_UNEXPECTED_JSON_KEY("unknown rpc::variant JSON tag");
            }
            else
            {
                using alternative_type = rpc::variant_alternative_t<Index, Variant>;
                if (std::strcmp(key, rpc::variant_alternative_tag<alternative_type>::value) == 0)
                {
                    alternative_type alternative{};
                    ar & alternative;
                    value = std::move(alternative);
                    return ar;
                }
                return load_alternative_by_tag<F, Archive, Variant, Index + 1U>(ar, key, value);
            }
        }
    } // namespace rpc_variant_detail

    template<std::size_t F, typename... Types>
    struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, rpc::variant<Types...>>
    {
        template<typename Archive>
        static Archive& save(
            Archive& ar,
            const rpc::variant<Types...>& value)
        {
            if (value.valueless_by_exception())
                throw std::runtime_error("Cannot serialize valueless rpc::variant");

            const auto index = value.index();
            if constexpr (F & yas::json)
            {
                ar.write("{", 1);
                rpc_variant_detail::save_alternative_tagged<F>(ar, value);
                ar.write("}", 1);
            }
            else
            {
                if (index > std::numeric_limits<std::uint8_t>::max())
                    throw std::runtime_error("Too many rpc::variant alternatives");
                ar.write(static_cast<std::uint8_t>(index));
                rpc_variant_detail::save_alternative<F>(ar, value);
            }

            return ar;
        }

        template<typename Archive>
        static Archive& load(
            Archive& ar,
            rpc::variant<Types...>& value)
        {
            // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays): YAS JSON validation macro uses an internal stack array.
            if constexpr (F & yas::json)
            {
                if constexpr (!(F & yas::compacted))
                    json_skipws(ar);

                __YAS_THROW_IF_BAD_JSON_CHARS(ar, "{");

                if constexpr (!(F & yas::compacted))
                    json_skipws(ar);

                // Match YAS's own object key buffer size — tags are IDL type
                // names and there is no enforced upper bound on struct name
                // length. A short buffer here would silently truncate the
                // key and overflow past `key` (json_read_key does not bound
                // its NUL write to the size).
                std::array<char, 1024> key{};
                json_read_key(ar, key.data(), key.size());

                rpc_variant_detail::load_alternative_by_tag<F>(ar, key.data(), value);

                if constexpr (!(F & yas::compacted))
                    json_skipws(ar);

                __YAS_THROW_IF_BAD_JSON_CHARS(ar, "}");
            }
            // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
            else
            {
                std::uint8_t index = 0;
                ar & index;
                rpc_variant_detail::load_alternative<F>(ar, static_cast<std::size_t>(index), value);
            }

            return ar;
        }
    };
} // namespace yas::detail
