/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <yas/detail/config/config.hpp>
#include <yas/detail/io/serialization_exceptions.hpp>
#include <yas/detail/type_traits/type_traits.hpp>
#include <yas/detail/type_traits/serializer.hpp>
#include <yas/detail/tools/json_tools.hpp>
#include <yas/object.hpp>

namespace rpc
{
    template<typename T> class id;

    // IDL-facing optional with natural JSON configuration semantics.
    //
    // Wire format:
    // - YAS binary/compressed binary: one presence byte followed by T only when present.
    // - YAS JSON as an object member: absent optional values are omitted from the object.
    // - YAS JSON as a standalone value: absent values serialise as null.
    // - Protobuf/nanopb: generated code maps this to presence-bearing wrapper messages.
    template<typename T> class optional
    {
    public:
        using value_type = T;

    private:
        template<typename U>
        static constexpr bool is_value_candidate
            = !std::is_same_v<std::decay_t<U>, optional> && !std::is_same_v<std::decay_t<U>, std::nullopt_t>;

        template<typename U>
        static constexpr bool can_construct_from = is_value_candidate<U> && std::is_constructible_v<T, U&&>;

        template<typename U>
        static constexpr bool can_assign_from = can_construct_from<U> && std::is_assignable_v<T&, U&&>;

    public:
        constexpr optional() noexcept = default;
        constexpr optional(std::nullopt_t) noexcept { }

        optional(const T& value)
            : value_(value)
        {
        }

        optional(T&& value)
            : value_(std::move(value))
        {
        }

        template<
            typename U,
            std::enable_if_t<
                can_construct_from<U>,
                int> = 0>
        optional(U&& value)
            : value_(std::forward<U>(value))
        {
        }

        optional& operator=(std::nullopt_t) noexcept
        {
            reset();
            return *this;
        }

        optional& operator=(const T& value)
        {
            value_ = value;
            return *this;
        }

        optional& operator=(T&& value)
        {
            value_ = std::move(value);
            return *this;
        }

        template<
            typename U,
            std::enable_if_t<
                can_assign_from<U>,
                int> = 0>
        optional& operator=(U&& value)
        {
            if (value_)
                *value_ = std::forward<U>(value);
            else
                value_.emplace(std::forward<U>(value));
            return *this;
        }

        [[nodiscard]] constexpr bool has_value() const noexcept { return value_.has_value(); }
        explicit constexpr operator bool() const noexcept { return has_value(); }

        [[nodiscard]] constexpr const T& value() const& { return value_.value(); }
        [[nodiscard]] constexpr T& value() & { return value_.value(); }
        [[nodiscard]] constexpr const T&& value() const&& { return std::move(value_).value(); }
        [[nodiscard]] constexpr T&& value() && { return std::move(value_).value(); }

        [[nodiscard]] constexpr const T& operator*() const& noexcept { return *value_; }
        [[nodiscard]] constexpr T& operator*() & noexcept { return *value_; }
        [[nodiscard]] constexpr const T&& operator*() const&& noexcept { return std::move(*value_); }
        [[nodiscard]] constexpr T&& operator*() && noexcept { return std::move(*value_); }

        [[nodiscard]] constexpr const T* operator->() const noexcept { return std::addressof(*value_); }
        [[nodiscard]] constexpr T* operator->() noexcept { return std::addressof(*value_); }

        void reset() noexcept { value_.reset(); }

        template<typename... Args> T& emplace(Args&&... args) { return value_.emplace(std::forward<Args>(args)...); }

    private:
        std::optional<T> value_;
    };

    template<typename T>
    [[nodiscard]] constexpr bool operator==(
        const optional<T>& lhs,
        const optional<T>& rhs)
    {
        if (lhs.has_value() != rhs.has_value())
            return false;

        if (!lhs.has_value())
            return true;

        return lhs.value() == rhs.value();
    }

    template<typename T>
    [[nodiscard]] constexpr bool operator!=(
        const optional<T>& lhs,
        const optional<T>& rhs)
    {
        return !(lhs == rhs);
    }

    template<typename T>
    [[nodiscard]] constexpr bool operator==(
        const optional<T>& lhs,
        std::nullopt_t) noexcept
    {
        return !lhs.has_value();
    }

    template<typename T>
    [[nodiscard]] constexpr bool operator!=(
        const optional<T>& lhs,
        std::nullopt_t) noexcept
    {
        return lhs.has_value();
    }

    template<typename T> class id<optional<T>>
    {
    public:
        static constexpr uint64_t get(uint64_t rpc_version)
        {
            auto value = 0x6B2F3C9D4E5871A5ull;
            value ^= id<T>::get(rpc_version);
            return (value << 1U) | (value >> 63U);
        }
    };
} // namespace rpc

namespace yas::detail
{
    // Canopy's YAS fork calls this trait from named JSON object serialisation.
    //
    // This is intentionally separate from the rpc::optional<T> value serializer
    // below.  The value serializer can write "null" for a standalone empty
    // optional, but only the surrounding YAS object serializer owns the member
    // key and can omit the whole "field": value pair.
    template<typename T> struct json_optional_field_traits<rpc::optional<T>>
    {
        static constexpr bool optional = true;

        static bool has_value(const rpc::optional<T>& value) noexcept { return value.has_value(); }
        static void reset(rpc::optional<T>& value) noexcept { value.reset(); }
    };

    template<std::size_t F, typename T>
    struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, rpc::optional<T>>
    {
        template<typename Archive>
        static Archive& save(
            Archive& ar,
            const rpc::optional<T>& value)
        {
            if constexpr (F & yas::json)
            {
                if (!value.has_value())
                {
                    ar.write("null", 4);
                    return ar;
                }
                return ar & value.value();
            }
            else
            {
                const bool present = value.has_value();
                ar.write(present);
                if (present)
                    ar & value.value();
                return ar;
            }
        }

        template<typename Archive>
        static Archive& load(
            Archive& ar,
            rpc::optional<T>& value)
        {
            if constexpr (F & yas::json)
            {
                if constexpr (!(F & yas::compacted))
                    json_skipws(ar);

                if (ar.peekch() == 'n')
                {
                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "null");
                    value.reset();
                    return ar;
                }

                T loaded{};
                ar & loaded;
                value = std::move(loaded);
                return ar;
            }
            else
            {
                bool present = false;
                ar.read(present);
                if (!present)
                {
                    value.reset();
                    return ar;
                }

                T loaded{};
                ar & loaded;
                value = std::move(loaded);
                return ar;
            }
        }
    };
} // namespace yas::detail
