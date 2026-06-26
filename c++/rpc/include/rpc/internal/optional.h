/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
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

    struct null_type
    {
    };

    inline constexpr null_type null{};

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

    // IDL-facing nullable value. Presence is owned by the containing object or
    // parameter; this type only represents a concrete value versus JSON null.
    template<typename T> class nullable : public optional<T>
    {
    public:
        using optional<T>::optional;
        using optional<T>::operator=;

        constexpr nullable() noexcept = default;
        constexpr nullable(std::nullopt_t) noexcept
            : optional<T>(std::nullopt)
        {
        }
    };

    template<typename T> struct is_nullable : std::false_type
    {
    };

    template<typename T> struct is_nullable<nullable<T>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_nullable_v = is_nullable<T>::value;

    // IDL-facing tri-state value for optional nullable JSON members:
    // absent, present null, or present concrete value.
    template<typename T> class nullable_optional
    {
    public:
        using value_type = T;

    private:
        enum class state_type : std::uint8_t
        {
            absent = 0,
            null_value = 1,
            value = 2
        };

        template<typename U>
        static constexpr bool is_value_candidate
            = !std::is_same_v<std::decay_t<U>, nullable_optional> && !std::is_same_v<std::decay_t<U>, nullable<T>>
              && !std::is_same_v<std::decay_t<U>, std::nullopt_t> && !std::is_same_v<std::decay_t<U>, null_type>;

        template<typename U>
        static constexpr bool can_construct_from = is_value_candidate<U> && std::is_constructible_v<T, U&&>;

        template<typename U>
        static constexpr bool can_assign_from = can_construct_from<U> && std::is_assignable_v<T&, U&&>;

    public:
        constexpr nullable_optional() noexcept = default;
        constexpr nullable_optional(std::nullopt_t) noexcept { }

        constexpr nullable_optional(null_type) noexcept
            : state_(state_type::null_value)
        {
        }

        nullable_optional(const nullable<T>& value)
        {
            if (value.has_value())
                emplace(value.value());
            else
                set_null();
        }

        nullable_optional(nullable<T>&& value)
        {
            if (value.has_value())
                emplace(std::move(value).value());
            else
                set_null();
        }

        nullable_optional(const T& value)
            : state_(state_type::value)
            , value_(value)
        {
        }

        nullable_optional(T&& value)
            : state_(state_type::value)
            , value_(std::move(value))
        {
        }

        template<
            typename U,
            std::enable_if_t<
                can_construct_from<U>,
                int> = 0>
        nullable_optional(U&& value)
            : state_(state_type::value)
            , value_(std::forward<U>(value))
        {
        }

        nullable_optional& operator=(std::nullopt_t) noexcept
        {
            reset();
            return *this;
        }

        nullable_optional& operator=(null_type) noexcept
        {
            set_null();
            return *this;
        }

        nullable_optional& operator=(const nullable<T>& value)
        {
            if (value.has_value())
                *this = value.value();
            else
                set_null();
            return *this;
        }

        nullable_optional& operator=(nullable<T>&& value)
        {
            if (value.has_value())
                *this = std::move(value).value();
            else
                set_null();
            return *this;
        }

        nullable_optional& operator=(const T& value)
        {
            value_ = value;
            state_ = state_type::value;
            return *this;
        }

        nullable_optional& operator=(T&& value)
        {
            value_ = std::move(value);
            state_ = state_type::value;
            return *this;
        }

        template<
            typename U,
            std::enable_if_t<
                can_assign_from<U>,
                int> = 0>
        nullable_optional& operator=(U&& value)
        {
            if (value_)
                *value_ = std::forward<U>(value);
            else
                value_.emplace(std::forward<U>(value));
            state_ = state_type::value;
            return *this;
        }

        [[nodiscard]] constexpr bool is_absent() const noexcept { return state_ == state_type::absent; }
        [[nodiscard]] constexpr bool is_null() const noexcept { return state_ == state_type::null_value; }
        [[nodiscard]] constexpr bool is_present() const noexcept { return !is_absent(); }
        [[nodiscard]] constexpr bool has_value() const noexcept { return state_ == state_type::value; }
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

        void reset() noexcept
        {
            state_ = state_type::absent;
            value_.reset();
        }

        void set_null() noexcept
        {
            state_ = state_type::null_value;
            value_.reset();
        }

        template<typename... Args> T& emplace(Args&&... args)
        {
            auto& value = value_.emplace(std::forward<Args>(args)...);
            state_ = state_type::value;
            return value;
        }

    private:
        state_type state_ = state_type::absent;
        std::optional<T> value_;
    };

    template<typename T> struct is_nullable_optional : std::false_type
    {
    };

    template<typename T> struct is_nullable_optional<nullable_optional<T>> : std::true_type
    {
    };

    template<typename T> inline constexpr bool is_nullable_optional_v = is_nullable_optional<T>::value;

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

    template<typename T>
    [[nodiscard]] constexpr bool operator==(
        const nullable_optional<T>& lhs,
        const nullable_optional<T>& rhs)
    {
        if (lhs.is_absent() || rhs.is_absent())
            return lhs.is_absent() && rhs.is_absent();

        if (lhs.is_null() || rhs.is_null())
            return lhs.is_null() && rhs.is_null();

        return lhs.value() == rhs.value();
    }

    template<typename T>
    [[nodiscard]] constexpr bool operator!=(
        const nullable_optional<T>& lhs,
        const nullable_optional<T>& rhs)
    {
        return !(lhs == rhs);
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

    template<typename T> class id<nullable<T>>
    {
    public:
        static constexpr uint64_t get(uint64_t rpc_version) { return id<optional<T>>::get(rpc_version); }
    };

    template<typename T> class id<nullable_optional<T>>
    {
    public:
        static constexpr uint64_t get(uint64_t rpc_version)
        {
            auto value = 0x4E554C4C4F50544Full;
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

    template<typename T> struct json_optional_field_traits<rpc::nullable_optional<T>>
    {
        static constexpr bool optional = true;

        static bool has_value(const rpc::nullable_optional<T>& value) noexcept { return value.is_present(); }
        static void reset(rpc::nullable_optional<T>& value) noexcept { value.reset(); }
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
                    if constexpr (rpc::is_nullable_v<T>)
                    {
                        T loaded{};
                        ar & loaded;
                        value = std::move(loaded);
                    }
                    else
                    {
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "null");
                        value.reset();
                    }
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

    template<std::size_t F, typename T>
    struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, rpc::nullable<T>>
        : serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, rpc::optional<T>>
    {
    };

    template<std::size_t F, typename T>
    struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, rpc::nullable_optional<T>>
    {
        template<typename Archive>
        static Archive& save(
            Archive& ar,
            const rpc::nullable_optional<T>& value)
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
                const std::uint8_t state = value.is_absent() ? 0 : (value.is_null() ? 1 : 2);
                ar.write(state);
                if (value.has_value())
                    ar & value.value();
                return ar;
            }
        }

        template<typename Archive>
        static Archive& load(
            Archive& ar,
            rpc::nullable_optional<T>& value)
        {
            if constexpr (F & yas::json)
            {
                if constexpr (!(F & yas::compacted))
                    json_skipws(ar);

                if (ar.peekch() == 'n')
                {
                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "null");
                    value.set_null();
                    return ar;
                }

                T loaded{};
                ar & loaded;
                value = std::move(loaded);
                return ar;
            }
            else
            {
                std::uint8_t state = 0;
                ar.read(state);
                switch (state)
                {
                case 0:
                    value.reset();
                    return ar;
                case 1:
                    value.set_null();
                    return ar;
                case 2:
                {
                    T loaded{};
                    ar & loaded;
                    value = std::move(loaded);
                    return ar;
                }
                default:
                    throw std::runtime_error("Invalid nullable_optional state in YAS stream");
                }
            }
        }
    };
} // namespace yas::detail
