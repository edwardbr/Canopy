/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <rpc/internal/build_modifiers.h>

YAS_WARNINGS_PUSH
#include <yas/binary_iarchive.hpp>
#include <yas/binary_oarchive.hpp>
#include <yas/detail/io/serialization_exceptions.hpp>
#include <yas/detail/tools/json_tools.hpp>
#include <yas/json_iarchive.hpp>
#include <yas/json_oarchive.hpp>
#include <yas/mem_streams.hpp>
#include <yas/serialize.hpp>
#include <yas/std_types.hpp>
YAS_WARNINGS_POP

#include <fmt/format.h>
#include <rpc/rpc.h>
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
#  include <rpc/serialization/canonical_crypto.h>
#endif

namespace json
{
    inline namespace v1
    {
        class object;
        class array;
        class map;

        class number
        {
        public:
            enum class type : uint8_t
            {
                signed_integer = 0,
                unsigned_integer = 1,
                floating = 2
            };

            number() = default;
            number(int64_t value)
                : value_(value)
            {
            }
            number(int32_t value)
                : value_(static_cast<int64_t>(value))
            {
            }
            number(int16_t value)
                : value_(static_cast<int64_t>(value))
            {
            }
            number(int8_t value)
                : value_(static_cast<int64_t>(value))
            {
            }
            number(uint64_t value)
                : value_(value)
            {
            }
            number(uint32_t value)
                : value_(static_cast<uint64_t>(value))
            {
            }
            number(uint16_t value)
                : value_(static_cast<uint64_t>(value))
            {
            }
            number(uint8_t value)
                : value_(static_cast<uint64_t>(value))
            {
            }
            number(double value)
                : value_(value)
                , lexical_(format_double(value))
            {
                if (!std::isfinite(value))
                    throw std::out_of_range("JSON numbers cannot be NaN or infinity");
            }
            explicit number(std::string lexical)
                : lexical_(std::move(lexical))
            {
                if (!is_strict_json_number(lexical_))
                    throw std::invalid_argument("invalid JSON number");
                value_ = parse_floating(lexical_);
            }

            [[nodiscard]] type get_type() const
            {
                if (std::holds_alternative<int64_t>(value_))
                    return type::signed_integer;
                if (std::holds_alternative<uint64_t>(value_))
                    return type::unsigned_integer;
                return type::floating;
            }

            [[nodiscard]] int64_t as_int64() const
            {
                if (const auto* value = std::get_if<int64_t>(&value_))
                    return *value;
                if (const auto* value = std::get_if<uint64_t>(&value_))
                {
                    if (*value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                        throw std::out_of_range("number does not fit in int64_t");
                    return static_cast<int64_t>(*value);
                }

                const auto value = std::get<double>(value_);
                if (value < static_cast<double>(std::numeric_limits<int64_t>::min())
                    || value >= static_cast<double>(std::numeric_limits<int64_t>::max()))
                {
                    throw std::out_of_range("floating JSON number does not fit in int64_t");
                }
                const auto converted = static_cast<int64_t>(value);
                if (static_cast<double>(converted) != value)
                    throw std::out_of_range("floating JSON number is not an int64_t");
                return converted;
            }

            [[nodiscard]] uint64_t as_uint64() const
            {
                if (const auto* value = std::get_if<uint64_t>(&value_))
                    return *value;
                if (const auto* value = std::get_if<int64_t>(&value_))
                {
                    if (*value < 0)
                        throw std::out_of_range("negative number does not fit in uint64_t");
                    return static_cast<uint64_t>(*value);
                }

                const auto value = std::get<double>(value_);
                if (value < 0 || value >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
                    throw std::out_of_range("floating JSON number does not fit in uint64_t");
                const auto converted = static_cast<uint64_t>(value);
                if (static_cast<double>(converted) != value)
                    throw std::out_of_range("floating JSON number is not a uint64_t");
                return converted;
            }

            [[nodiscard]] double as_double() const
            {
                if (const auto* value = std::get_if<int64_t>(&value_))
                    return static_cast<double>(*value);
                if (const auto* value = std::get_if<uint64_t>(&value_))
                    return static_cast<double>(*value);
                return std::get<double>(value_);
            }

            template<typename T> [[nodiscard]] T as_integral() const
            {
                static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
                if constexpr (std::is_signed_v<T>)
                {
                    const auto value = as_int64();
                    if (value < static_cast<int64_t>(std::numeric_limits<T>::min())
                        || value > static_cast<int64_t>(std::numeric_limits<T>::max()))
                    {
                        throw std::out_of_range("number does not fit requested signed integer type");
                    }
                    return static_cast<T>(value);
                }
                else
                {
                    const auto value = as_uint64();
                    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
                        throw std::out_of_range("number does not fit requested unsigned integer type");
                    return static_cast<T>(value);
                }
            }

            [[nodiscard]] std::string to_json_string() const
            {
                if (const auto* value = std::get_if<int64_t>(&value_))
                    return std::to_string(*value);
                if (const auto* value = std::get_if<uint64_t>(&value_))
                    return std::to_string(*value);
                if (!lexical_.empty())
                    return lexical_;
                return format_double(std::get<double>(value_));
            }

            [[nodiscard]] static number parse(std::string_view token)
            {
                if (token.empty())
                    throw std::invalid_argument("empty JSON number");
                if (!is_strict_json_number(token))
                    throw std::invalid_argument("invalid JSON number");

                const bool is_floating = token.find_first_of(".eE") != std::string_view::npos;
                if (!is_floating)
                {
                    if (token.front() == '-')
                    {
                        int64_t parsed = 0;
                        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), parsed);
                        if (ec == std::errc{} && ptr == token.data() + token.size())
                            return number(parsed);
                    }
                    else
                    {
                        uint64_t parsed = 0;
                        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), parsed);
                        if (ec == std::errc{} && ptr == token.data() + token.size())
                            return number(parsed);
                    }
                }

                return number(std::string(token));
            }

            void protobuf_serialise(std::vector<char>& buffer) const;
            void protobuf_deserialise(const std::vector<char>& buffer);
            void nanopb_serialise(std::vector<char>& buffer) const;
            void nanopb_deserialise(const std::vector<char>& buffer);
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            bool canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const;
            bool canonical_crypto_read_from(rpc::canonical_crypto_reader& reader);
#endif

        private:
            std::variant<int64_t, uint64_t, double> value_{int64_t{0}};
            std::string lexical_;

            [[nodiscard]] static bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

            [[nodiscard]] static bool is_strict_json_number(std::string_view token)
            {
                size_t i = 0;
                if (token.empty())
                    return false;

                if (token[i] == '-')
                {
                    ++i;
                    if (i == token.size())
                        return false;
                }

                if (token[i] == '0')
                {
                    ++i;
                }
                else if (token[i] >= '1' && token[i] <= '9')
                {
                    while (i < token.size() && is_digit(token[i]))
                        ++i;
                }
                else
                {
                    return false;
                }

                if (i < token.size() && token[i] == '.')
                {
                    ++i;
                    const auto fraction_start = i;
                    while (i < token.size() && is_digit(token[i]))
                        ++i;
                    if (i == fraction_start)
                        return false;
                }

                if (i < token.size() && (token[i] == 'e' || token[i] == 'E'))
                {
                    ++i;
                    if (i < token.size() && (token[i] == '+' || token[i] == '-'))
                        ++i;
                    const auto exponent_start = i;
                    while (i < token.size() && is_digit(token[i]))
                        ++i;
                    if (i == exponent_start)
                        return false;
                }

                return i == token.size();
            }

            [[nodiscard]] static double parse_floating(const std::string& lexical)
            {
                // Prefer std::from_chars for locale independence — std::strtod
                // honours the active C locale and would misparse JSON like
                // "1.5" on a de_DE host. from_chars<double> is C++17 but the
                // floating-point overload only landed in libstdc++ 11 / MSVC
                // 19.24, so we fall back to a locale-classic strtod when it
                // isn't available.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611 && (!defined(__GLIBCXX__) || __GLIBCXX__ >= 20210408)  \
    && (!defined(_LIBCPP_VERSION) || _LIBCPP_VERSION >= 14000)
                double value = 0.0;
                const auto* first = lexical.data();
                const auto* last = first + lexical.size();
                const auto result = std::from_chars(first, last, value);
                if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(value))
                    throw std::out_of_range("invalid JSON floating-point number");
                return value;
#else
                errno = 0;
                char* end = nullptr;
                const double value = std::strtod(lexical.c_str(), &end);
                if (errno == ERANGE || end != lexical.c_str() + lexical.size() || !std::isfinite(value))
                    throw std::out_of_range("invalid JSON floating-point number");
                return value;
#endif
            }

            [[nodiscard]] static std::string format_double(double value)
            {
                if (!std::isfinite(value))
                    throw std::out_of_range("invalid JSON floating-point number");
                return fmt::format("{:.{}g}", value, std::numeric_limits<double>::max_digits10);
            }

            friend bool operator==(
                const number& lhs,
                const number& rhs);
        };

        // Compare two JSON numbers by mathematical value rather than by the
        // active variant alternative. 1 (int64) and 1.0 (double) round-trip
        // through JSON as equivalent and most callers (DOM diffing, set
        // dedupe, schema const/enum/uniqueItems) want them treated as equal.
        // Strict identity-of-representation is available via the typed
        // accessors on `number` itself.
        inline bool operator==(
            const number& lhs,
            const number& rhs)
        {
            // Same active alternative: variant equality is already lenient
            // within the same type.
            if (lhs.value_.index() == rhs.value_.index())
                return lhs.value_ == rhs.value_;

            // Cross-type: route signed/unsigned through their natural
            // comparison and only fall back to double when at least one side
            // is genuinely floating-point. The double conversion is the only
            // path that can lose precision, so it is deliberately last.
            const auto lhs_type = lhs.get_type();
            const auto rhs_type = rhs.get_type();
            if (lhs_type == number::type::signed_integer && rhs_type == number::type::unsigned_integer)
            {
                const auto lhs_value = lhs.as_int64();
                return lhs_value >= 0 && static_cast<uint64_t>(lhs_value) == rhs.as_uint64();
            }
            if (lhs_type == number::type::unsigned_integer && rhs_type == number::type::signed_integer)
            {
                const auto rhs_value = rhs.as_int64();
                return rhs_value >= 0 && lhs.as_uint64() == static_cast<uint64_t>(rhs_value);
            }
            return lhs.as_double() == rhs.as_double();
        }

        inline bool operator!=(
            const number& lhs,
            const number& rhs)
        {
            return !(lhs == rhs);
        }

        class object
        {
        public:
            using container_type
                = std::variant<std::string, number, bool, std::nullptr_t, std::unique_ptr<const array>, std::unique_ptr<const map>>;

            static constexpr size_t STRING_TYPE_INDEX = 0;
            static constexpr size_t DOUBLE_TYPE_INDEX = 1;
            static constexpr size_t NUMBER_TYPE_INDEX = DOUBLE_TYPE_INDEX;
            static constexpr size_t BOOL_TYPE_INDEX = 2;
            static constexpr size_t NULL_TYPE_INDEX = 3;
            static constexpr size_t ARRAY_TYPE_INDEX = 4;
            static constexpr size_t MAP_TYPE_INDEX = 5;

            enum class type : uint8_t
            {
                string_type = STRING_TYPE_INDEX,
                number_type = NUMBER_TYPE_INDEX,
                double_type = NUMBER_TYPE_INDEX,
                bool_type = BOOL_TYPE_INDEX,
                null_type = NULL_TYPE_INDEX,
                array_type = ARRAY_TYPE_INDEX,
                map_type = MAP_TYPE_INDEX
            };

            object()
                : value_(nullptr)
            {
            }
            explicit object(std::string value)
                : value_(std::move(value))
            {
            }
            explicit object(const char* value)
                : value_(std::string(value))
            {
            }
            explicit object(number value)
                : value_(std::move(value))
            {
            }
            explicit object(int64_t value)
                : value_(number(value))
            {
            }
            explicit object(int32_t value)
                : value_(number(static_cast<int64_t>(value)))
            {
            }
            explicit object(int16_t value)
                : value_(number(static_cast<int64_t>(value)))
            {
            }
            explicit object(int8_t value)
                : value_(number(static_cast<int64_t>(value)))
            {
            }
            explicit object(uint64_t value)
                : value_(number(value))
            {
            }
            explicit object(uint32_t value)
                : value_(number(static_cast<uint64_t>(value)))
            {
            }
            explicit object(uint16_t value)
                : value_(number(static_cast<uint64_t>(value)))
            {
            }
            explicit object(uint8_t value)
                : value_(number(static_cast<uint64_t>(value)))
            {
            }
            explicit object(double value)
                : value_(number(value))
            {
            }
            explicit object(bool value)
                : value_(value)
            {
            }
            explicit object(std::nullptr_t)
                : value_(nullptr)
            {
            }
            explicit object(const array& value);
            explicit object(array&& value);
            explicit object(std::unique_ptr<array>&& value);
            explicit object(const map& value);
            explicit object(map&& value);
            explicit object(std::unique_ptr<map>&& value);

            ~object();
            object(const object& other);
            object(object&&) noexcept;

            object& operator=(const object& other);
            object& operator=(object&&) noexcept;

            object& operator=(std::string value)
            {
                value_ = std::move(value);
                return *this;
            }
            object& operator=(const char* value)
            {
                value_ = std::string(value);
                return *this;
            }
            object& operator=(number value)
            {
                value_ = std::move(value);
                return *this;
            }
            object& operator=(int64_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(int32_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(int16_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(int8_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(uint64_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(uint32_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(uint16_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(uint8_t value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(double value)
            {
                value_ = number(value);
                return *this;
            }
            object& operator=(bool value)
            {
                value_ = value;
                return *this;
            }
            object& operator=(std::nullptr_t)
            {
                value_ = nullptr;
                return *this;
            }
            object& operator=(const array& value);
            object& operator=(array&& value);
            object& operator=(std::unique_ptr<array>&& value);
            object& operator=(const map& value);
            object& operator=(map&& value);
            object& operator=(std::unique_ptr<map>&& value);

            [[nodiscard]] type get_type() const { return static_cast<type>(value_.index()); }
            [[nodiscard]] const container_type& raw() const { return value_; }

            [[nodiscard]] const array& as_array() const;
            [[nodiscard]] const map& as_map() const;

            template<typename T> [[nodiscard]] T get() const;

            void protobuf_serialise(std::vector<char>& buffer) const;
            void protobuf_deserialise(const std::vector<char>& buffer);
            void nanopb_serialise(std::vector<char>& buffer) const;
            void nanopb_deserialise(const std::vector<char>& buffer);
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            bool canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const;
            bool canonical_crypto_read_from(rpc::canonical_crypto_reader& reader);
#endif

        private:
            container_type value_;

            friend bool operator==(
                const object& lhs,
                const object& rhs);
        };

        class map : public std::unordered_map<std::string, object>
        {
        public:
            using base = std::unordered_map<std::string, object>;

            struct entry
            {
                std::string key;
                object value;

                template<typename Value>
                entry(
                    std::string entry_key,
                    Value&& entry_value)
                    : key(std::move(entry_key))
                    , value(std::forward<Value>(entry_value))
                {
                }
            };

            map() = default;
            map(const map&) = default;
            map(map&&) = default;
            map(std::initializer_list<entry> init)
            {
                base::reserve(init.size());
                for (const auto& item : init)
                    base::emplace(item.key, item.value);
            }

            map& operator=(const map&) = default;
            map& operator=(map&&) = default;

            void protobuf_serialise(std::vector<char>& buffer) const;
            void protobuf_deserialise(const std::vector<char>& buffer);
            void nanopb_serialise(std::vector<char>& buffer) const;
            void nanopb_deserialise(const std::vector<char>& buffer);
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            bool canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const;
            bool canonical_crypto_read_from(rpc::canonical_crypto_reader& reader);
#endif
        };

        class array : public std::vector<object>
        {
        public:
            using base = std::vector<object>;

            struct element
            {
                object value;

                element() = default;
                element(const element&) = default;
                element(element&&) noexcept = default;

                template<
                    typename Value,
                    std::enable_if_t<
                        !std::is_same_v<
                            std::decay_t<Value>,
                            element>,
                        int> = 0>
                element(Value&& element_value)
                    : value(std::forward<Value>(element_value))
                {
                }

                element& operator=(const element&) = default;
                element& operator=(element&&) noexcept = default;
            };

            array() = default;
            array(const array&) = default;
            array(array&&) = default;
            array(std::initializer_list<element> init)
            {
                base::reserve(init.size());
                for (const auto& item : init)
                    base::push_back(item.value);
            }

            array& operator=(const array&) = default;
            array& operator=(array&&) = default;

            void protobuf_serialise(std::vector<char>& buffer) const;
            void protobuf_deserialise(const std::vector<char>& buffer);
            void nanopb_serialise(std::vector<char>& buffer) const;
            void nanopb_deserialise(const std::vector<char>& buffer);
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            bool canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const;
            bool canonical_crypto_read_from(rpc::canonical_crypto_reader& reader);
#endif
        };

        inline object::object(const array& value)
            : value_(std::make_unique<array>(value))
        {
        }

        inline object::object(array&& value)
            : value_(std::make_unique<array>(std::move(value)))
        {
        }

        inline object::object(std::unique_ptr<array>&& value)
            : value_(std::move(value))
        {
        }

        inline object::object(const map& value)
            : value_(std::make_unique<map>(value))
        {
        }

        inline object::object(map&& value)
            : value_(std::make_unique<map>(std::move(value)))
        {
        }

        inline object::object(std::unique_ptr<map>&& value)
            : value_(std::move(value))
        {
        }

        inline object::~object() = default;
        inline object::object(const object& other)
        {
            *this = other;
        }
        inline object::object(object&&) noexcept = default;

        inline object& object::operator=(const object& other)
        {
            std::visit(
                [this](const auto& value)
                {
                    using value_type = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<value_type, std::unique_ptr<const array>>)
                    {
                        if (!value)
                            throw std::invalid_argument("invalid JSON array");
                        value_ = std::make_unique<array>(*value);
                    }
                    else if constexpr (std::is_same_v<value_type, std::unique_ptr<const map>>)
                    {
                        if (!value)
                            throw std::invalid_argument("invalid JSON map");
                        value_ = std::make_unique<map>(*value);
                    }
                    else
                    {
                        value_ = value;
                    }
                },
                other.value_);
            return *this;
        }

        inline object& object::operator=(object&&) noexcept = default;

        inline object& object::operator=(const array& value)
        {
            value_ = std::make_unique<array>(value);
            return *this;
        }

        inline object& object::operator=(array&& value)
        {
            value_ = std::make_unique<array>(std::move(value));
            return *this;
        }

        inline object& object::operator=(std::unique_ptr<array>&& value)
        {
            value_ = std::move(value);
            return *this;
        }

        inline object& object::operator=(const map& value)
        {
            value_ = std::make_unique<map>(value);
            return *this;
        }

        inline object& object::operator=(map&& value)
        {
            value_ = std::make_unique<map>(std::move(value));
            return *this;
        }

        inline object& object::operator=(std::unique_ptr<map>&& value)
        {
            value_ = std::move(value);
            return *this;
        }

        inline const array& object::as_array() const
        {
            const auto& value = std::get<std::unique_ptr<const array>>(value_);
            if (!value)
                throw std::invalid_argument("invalid JSON array");
            return *value;
        }

        inline const map& object::as_map() const
        {
            const auto& value = std::get<std::unique_ptr<const map>>(value_);
            if (!value)
                throw std::invalid_argument("invalid JSON map");
            return *value;
        }

        template<typename T> inline T object::get() const
        {
            if constexpr (std::is_same_v<T, std::string>)
                return std::get<std::string>(value_);
            else if constexpr (std::is_same_v<T, bool>)
                return std::get<bool>(value_);
            else if constexpr (std::is_same_v<T, number>)
                return std::get<number>(value_);
            else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
                return std::get<number>(value_).template as_integral<T>();
            else if constexpr (std::is_floating_point_v<T>)
                return static_cast<T>(std::get<number>(value_).as_double());
            else if constexpr (std::is_same_v<T, array>)
                return as_array();
            else if constexpr (std::is_same_v<T, map>)
                return as_map();
            else
            {
                static_assert(!sizeof(T*), "unsupported json::v1::object conversion");
            }
        }

        inline bool operator==(
            const object& lhs,
            const object& rhs)
        {
            if (lhs.get_type() != rhs.get_type())
                return false;

            if (lhs.get_type() == object::type::array_type)
                return lhs.as_array() == rhs.as_array();
            if (lhs.get_type() == object::type::map_type)
                return lhs.as_map() == rhs.as_map();
            return lhs.value_ == rhs.value_;
        }

        inline bool operator!=(
            const object& lhs,
            const object& rhs)
        {
            return !(lhs == rhs);
        }

        inline bool operator==(
            const array& lhs,
            const array& rhs)
        {
            return static_cast<const array::base&>(lhs) == static_cast<const array::base&>(rhs);
        }

        inline bool operator!=(
            const array& lhs,
            const array& rhs)
        {
            return !(lhs == rhs);
        }

        inline bool operator==(
            const map& lhs,
            const map& rhs)
        {
            return static_cast<const map::base&>(lhs) == static_cast<const map::base&>(rhs);
        }

        inline bool operator!=(
            const map& lhs,
            const map& rhs)
        {
            return !(lhs == rhs);
        }

        inline object parse(std::string_view text)
        {
            object value;
            YAS_WARNINGS_PUSH
            yas::load<yas::mem | yas::json | yas::no_header>(yas::intrusive_buffer{text.data(), text.size()}, value);
            YAS_WARNINGS_POP
            return value;
        }

        inline std::string dump(const object& value)
        {
            YAS_WARNINGS_PUSH
            yas::shared_buffer buffer = yas::save<yas::mem | yas::json | yas::no_header>(value);
            YAS_WARNINGS_POP
            return std::string(buffer.data.get(), buffer.data.get() + buffer.size);
        }
    } // namespace v1
} // namespace json

namespace yas
{
    namespace detail
    {
        inline bool json_number_char(char ch)
        {
            switch (ch)
            {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '-':
            case '+':
            case '.':
            case 'e':
            case 'E':
                return true;
            default:
                return false;
            }
        }

        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::number>
        {
            template<typename Archive>
            static Archive& save(
                Archive& ar,
                const json::v1::number& value)
            {
                if constexpr (F & yas::json)
                {
                    const auto text = value.to_json_string();
                    ar.write(text.data(), text.size());
                }
                else
                {
                    const auto type = static_cast<uint8_t>(value.get_type());
                    ar & type;
                    switch (value.get_type())
                    {
                    case json::v1::number::type::signed_integer:
                    {
                        auto integer = value.as_int64();
                        ar & integer;
                        break;
                    }
                    case json::v1::number::type::unsigned_integer:
                    {
                        auto integer = value.as_uint64();
                        ar & integer;
                        break;
                    }
                    case json::v1::number::type::floating:
                    {
                        auto text = value.to_json_string();
                        ar & text;
                        break;
                    }
                    }
                }
                return ar;
            }

            template<typename Archive>
            static Archive& load(
                Archive& ar,
                json::v1::number& value)
            {
                if constexpr (F & yas::json)
                {
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    std::string token;
                    char ch = ar.getch();
                    while (true)
                    {
                        if (!json_number_char(ch))
                            break;
                        token += ch;
                        if (ar.empty())
                            break;
                        ch = ar.peekch();
                        if (!json_number_char(ch))
                            break;
                        ar.getch();
                    }
                    value = json::v1::number::parse(token);
                }
                else
                {
                    uint8_t type = 0;
                    ar & type;
                    switch (static_cast<json::v1::number::type>(type))
                    {
                    case json::v1::number::type::signed_integer:
                    {
                        int64_t integer = 0;
                        ar & integer;
                        value = json::v1::number(integer);
                        break;
                    }
                    case json::v1::number::type::unsigned_integer:
                    {
                        uint64_t integer = 0;
                        ar & integer;
                        value = json::v1::number(integer);
                        break;
                    }
                    case json::v1::number::type::floating:
                    {
                        std::string text;
                        ar & text;
                        value = json::v1::number(std::move(text));
                        break;
                    }
                    default:
                        __YAS_THROW_EXCEPTION(::yas::serialization_exception, "invalid JSON number type");
                    }
                }
                return ar;
            }
        };

        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::object>
        {
            template<typename Archive>
            static Archive& save(
                Archive& ar,
                const json::v1::object& object)
            {
                if constexpr (!(F & yas::json))
                {
                    const auto type = static_cast<uint8_t>(object.get_type());
                    ar & type;
                }

                switch (object.get_type())
                {
                case json::v1::object::type::string_type:
                    if constexpr (F & yas::json)
                    {
                        const auto& value = object.get<std::string>();
                        ar.write("\"", 1);
                        save_string(ar, value.data(), value.size());
                        ar.write("\"", 1);
                    }
                    else
                    {
                        ar & object.get<std::string>();
                    }
                    break;
                case json::v1::object::type::number_type:
                    ar & object.get<json::v1::number>();
                    break;
                case json::v1::object::type::bool_type:
                    ar & object.get<bool>();
                    break;
                case json::v1::object::type::null_type:
                    if constexpr (F & yas::json)
                        ar.write("null", 4);
                    break;
                case json::v1::object::type::array_type:
                    ar & object.get<json::v1::array>();
                    break;
                case json::v1::object::type::map_type:
                    ar & object.get<json::v1::map>();
                    break;
                }

                return ar;
            }

            template<typename Archive>
            static Archive& load(
                Archive& ar,
                json::v1::object& object)
            {
                // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays): YAS JSON validation macro uses an internal stack array.
                if constexpr (F & yas::json)
                {
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    std::string str;
                    const char ch = ar.getch();
                    if (ch == '"')
                    {
                        load_string(str, ar);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "\"");
                        object = std::move(str);
                    }
                    else if (ch == 'n')
                    {
                        ar.ungetch(ch);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "null");
                        object = nullptr;
                    }
                    else if (ch == 't')
                    {
                        ar.ungetch(ch);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "true");
                        object = true;
                    }
                    else if (ch == 'f')
                    {
                        ar.ungetch(ch);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "false");
                        object = false;
                    }
                    else if (ch == '[')
                    {
                        ar.ungetch(ch);
                        json::v1::array arr;
                        ar & arr;
                        object = std::make_unique<json::v1::array>(std::move(arr));
                    }
                    else if (ch == '{')
                    {
                        ar.ungetch(ch);
                        auto obj = std::make_unique<json::v1::map>();
                        auto& map = *obj;
                        ar & map;
                        object = std::move(obj);
                    }
                    else
                    {
                        ar.ungetch(ch);
                        json::v1::number number;
                        ar & number;
                        object = number;
                    }
                }
                // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
                else
                {
                    uint8_t type = 0;
                    ar & type;
                    switch (static_cast<json::v1::object::type>(type))
                    {
                    case json::v1::object::type::string_type:
                    {
                        std::string value;
                        ar & value;
                        object = std::move(value);
                        break;
                    }
                    case json::v1::object::type::number_type:
                    {
                        json::v1::number value;
                        ar & value;
                        object = value;
                        break;
                    }
                    case json::v1::object::type::bool_type:
                    {
                        bool value = false;
                        ar & value;
                        object = value;
                        break;
                    }
                    case json::v1::object::type::null_type:
                        object = nullptr;
                        break;
                    case json::v1::object::type::array_type:
                    {
                        json::v1::array value;
                        ar & value;
                        object = value;
                        break;
                    }
                    case json::v1::object::type::map_type:
                    {
                        json::v1::map value;
                        ar & value;
                        object = value;
                        break;
                    }
                    default:
                        __YAS_THROW_EXCEPTION(::yas::serialization_exception, "invalid JSON object type");
                    }
                }
                return ar;
            }
        };

        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::array>
        {
            template<typename Archive>
            static Archive& save(
                Archive& ar,
                const json::v1::array& array)
            {
                if constexpr (F & yas::json)
                {
                    ar.write("[", 1);
                    bool first = true;
                    for (const auto& value : array)
                    {
                        if (!first)
                            ar.write(",", 1);
                        first = false;
                        ar & value;
                    }
                    ar.write("]", 1);
                }
                else
                {
                    ar.write_seq_size(array.size());
                    for (const auto& value : array)
                        ar & value;
                }
                return ar;
            }

            template<typename Archive>
            static Archive& load(
                Archive& ar,
                json::v1::array& array)
            {
                // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays): YAS JSON validation macro uses an internal stack array.
                if constexpr (F & yas::json)
                {
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "[");
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    if (ar.peekch() == ']')
                    {
                        ar.getch();
                        return ar;
                    }

                    while (true)
                    {
                        json::v1::object value;
                        ar & value;
                        array.emplace_back(std::move(value));

                        __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                        {
                            json_skipws(ar);
                        }

                        if (ar.peekch() == ']')
                            break;

                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, ",");
                        __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                        {
                            json_skipws(ar);
                        }
                    }
                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "]");
                }
                // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
                else
                {
                    const auto size = ar.read_seq_size();
                    array.clear();
                    array.reserve(size);
                    for (size_t i = 0; i < size; ++i)
                    {
                        json::v1::object value;
                        ar & value;
                        array.emplace_back(std::move(value));
                    }
                }
                return ar;
            }
        };

        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::map>
        {
            template<typename Archive>
            static Archive& save(
                Archive& ar,
                const json::v1::map& map)
            {
                if constexpr (F & yas::json)
                {
                    // Iterate keys in sorted order so dump() is byte-stable
                    // across runs / standard library versions. Matches the
                    // protobuf and canonical_crypto serialisers, which
                    // already sort to give deterministic output.
                    std::vector<const json::v1::map::base::value_type*> ordered;
                    ordered.reserve(map.size());
                    for (const auto& entry : map)
                        ordered.push_back(&entry);
                    std::sort(
                        ordered.begin(),
                        ordered.end(),
                        [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });

                    ar.write("{", 1);
                    bool first = true;
                    for (const auto* entry : ordered)
                    {
                        if (!first)
                            ar.write(",", 1);
                        first = false;
                        ar.write("\"", 1);
                        save_string(ar, entry->first.data(), entry->first.size());
                        ar.write("\":", 2);
                        ar & entry->second;
                    }
                    ar.write("}", 1);
                }
                else
                {
                    ar.write_seq_size(map.size());
                    for (const auto& [key, value] : map)
                        ar & key & value;
                }
                return ar;
            }

            template<typename Archive>
            static Archive& load(
                Archive& ar,
                json::v1::map& map)
            {
                // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays): YAS JSON validation macro uses an internal stack array.
                if constexpr (F & yas::json)
                {
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "{");
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    if (ar.peekch() == '}')
                    {
                        ar.getch();
                        return ar;
                    }

                    while (true)
                    {
                        std::string key;
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "\"");
                        load_string(key, ar);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "\"");
                        __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                        {
                            json_skipws(ar);
                        }
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, ":");
                        __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                        {
                            json_skipws(ar);
                        }

                        json::v1::object value;
                        ar & value;
                        map.emplace(std::move(key), std::move(value));

                        __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                        {
                            json_skipws(ar);
                        }

                        if (ar.peekch() == '}')
                            break;

                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, ",");
                        __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                        {
                            json_skipws(ar);
                        }
                    }
                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "}");
                }
                // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
                else
                {
                    const auto size = ar.read_seq_size();
                    map.clear();
                    for (size_t i = 0; i < size; ++i)
                    {
                        std::string key;
                        json::v1::object value;
                        ar & key & value;
                        map.emplace(std::move(key), std::move(value));
                    }
                }
                return ar;
            }
        };
    } // namespace detail
} // namespace yas

namespace json
{
    inline namespace v1
    {
        namespace detail
        {
            // Protobuf/nanopb use a compact tagged-union wire shape for JSON values.
            // This deliberately stays separate from the YAS binary and YAS JSON serializers above.
            namespace protobuf_wire
            {
                enum class wire_type : uint8_t
                {
                    varint = 0,
                    fixed64 = 1,
                    length_delimited = 2,
                    fixed32 = 5
                };

                inline void append_varint(
                    std::vector<char>& buffer,
                    uint64_t value)
                {
                    while (value >= 0x80)
                    {
                        buffer.push_back(static_cast<char>((value & 0x7F) | 0x80));
                        value >>= 7;
                    }
                    buffer.push_back(static_cast<char>(value));
                }

                inline uint64_t read_varint(
                    const char*& current,
                    const char* end)
                {
                    uint64_t value = 0;
                    uint8_t shift = 0;
                    while (current != end && shift < 64)
                    {
                        const auto byte = static_cast<uint8_t>(*current++);
                        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
                        if ((byte & 0x80) == 0)
                            return value;
                        shift += 7;
                    }
                    throw std::runtime_error("invalid protobuf varint");
                }

                inline uint64_t make_key(
                    uint32_t field_number,
                    wire_type type)
                {
                    return (static_cast<uint64_t>(field_number) << 3) | static_cast<uint8_t>(type);
                }

                inline void append_key(
                    std::vector<char>& buffer,
                    uint32_t field_number,
                    wire_type type)
                {
                    append_varint(buffer, make_key(field_number, type));
                }

                inline int64_t decode_zigzag(uint64_t value)
                {
                    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
                }

                inline uint64_t encode_zigzag(int64_t value)
                {
                    return (static_cast<uint64_t>(value) << 1) ^ (value < 0 ? std::numeric_limits<uint64_t>::max() : 0);
                }

                inline void append_fixed64(
                    std::vector<char>& buffer,
                    uint64_t value)
                {
                    for (uint8_t i = 0; i != 8; ++i)
                        buffer.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
                }

                inline uint64_t read_fixed64(
                    const char*& current,
                    const char* end)
                {
                    if (static_cast<size_t>(end - current) < 8)
                        throw std::runtime_error("truncated protobuf fixed64 field");

                    uint64_t value = 0;
                    for (uint8_t i = 0; i != 8; ++i)
                        value |= static_cast<uint64_t>(static_cast<uint8_t>(*current++)) << (i * 8);
                    return value;
                }

                inline void append_double(
                    std::vector<char>& buffer,
                    double value)
                {
                    uint64_t bits = 0;
                    std::memcpy(&bits, &value, sizeof(bits));
                    append_fixed64(buffer, bits);
                }

                inline double read_double(
                    const char*& current,
                    const char* end)
                {
                    const uint64_t bits = read_fixed64(current, end);
                    double value = 0.0;
                    std::memcpy(&value, &bits, sizeof(value));
                    return value;
                }

                inline void append_length_delimited(
                    std::vector<char>& buffer,
                    uint32_t field_number,
                    const char* data,
                    size_t size)
                {
                    append_key(buffer, field_number, wire_type::length_delimited);
                    append_varint(buffer, size);
                    if (size != 0)
                        buffer.insert(buffer.end(), data, data + size);
                }

                inline void append_length_delimited(
                    std::vector<char>& buffer,
                    uint32_t field_number,
                    const std::vector<char>& value)
                {
                    append_length_delimited(buffer, field_number, value.data(), value.size());
                }

                inline void append_string(
                    std::vector<char>& buffer,
                    uint32_t field_number,
                    const std::string& value)
                {
                    append_length_delimited(buffer, field_number, value.data(), value.size());
                }

                inline std::string_view read_length_delimited(
                    const char*& current,
                    const char* end)
                {
                    const auto size = read_varint(current, end);
                    if (size > static_cast<uint64_t>(end - current))
                        throw std::runtime_error("truncated protobuf length-delimited field");

                    const char* data = current;
                    current += size;
                    return {data, static_cast<size_t>(size)};
                }

                inline void skip_field(
                    const char*& current,
                    const char* end,
                    wire_type type)
                {
                    switch (type)
                    {
                    case wire_type::varint:
                        (void)read_varint(current, end);
                        break;
                    case wire_type::fixed64:
                        if (static_cast<size_t>(end - current) < 8)
                            throw std::runtime_error("truncated protobuf fixed64 field");
                        current += 8;
                        break;
                    case wire_type::length_delimited:
                        (void)read_length_delimited(current, end);
                        break;
                    case wire_type::fixed32:
                        if (static_cast<size_t>(end - current) < 4)
                            throw std::runtime_error("truncated protobuf fixed32 field");
                        current += 4;
                        break;
                    default:
                        throw std::runtime_error("invalid protobuf wire type");
                    }
                }

                inline void parse_key(
                    const char*& current,
                    const char* end,
                    uint32_t& field_number,
                    wire_type& type)
                {
                    const auto key = read_varint(current, end);
                    field_number = static_cast<uint32_t>(key >> 3);
                    type = static_cast<wire_type>(key & 0x7);
                    if (field_number == 0)
                        throw std::runtime_error("invalid protobuf field number");
                }

                inline const char* buffer_begin(const std::vector<char>& buffer)
                {
                    return buffer.empty() ? "" : buffer.data();
                }

                inline const char* view_begin(std::string_view view)
                {
                    return view.empty() ? "" : view.data();
                }
            } // namespace protobuf_wire
        } // namespace detail

        inline void number::protobuf_serialise(std::vector<char>& buffer) const
        {
            namespace wire = detail::protobuf_wire;
            buffer.clear();
            switch (get_type())
            {
            case number::type::signed_integer:
                wire::append_key(buffer, 1, wire::wire_type::varint);
                wire::append_varint(buffer, wire::encode_zigzag(as_int64()));
                break;
            case number::type::unsigned_integer:
                wire::append_key(buffer, 2, wire::wire_type::varint);
                wire::append_varint(buffer, as_uint64());
                break;
            case number::type::floating:
            {
                wire::append_key(buffer, 3, wire::wire_type::fixed64);
                wire::append_double(buffer, as_double());
                if (!lexical_.empty())
                    wire::append_string(buffer, 4, lexical_);
                break;
            }
            }
        }

        inline void number::protobuf_deserialise(const std::vector<char>& buffer)
        {
            namespace wire = detail::protobuf_wire;
            enum class decoded_type
            {
                signed_integer,
                unsigned_integer,
                floating,
                floating_lexical
            };

            auto type = decoded_type::signed_integer;
            int64_t signed_value = 0;
            uint64_t unsigned_value = 0;
            double floating_value = 0.0;
            std::string lexical_value;

            const char* current = wire::buffer_begin(buffer);
            const char* end = current + buffer.size();
            while (current != end)
            {
                uint32_t field_number = 0;
                wire::wire_type field_type{};
                wire::parse_key(current, end, field_number, field_type);

                switch (field_number)
                {
                case 1:
                    if (field_type != wire::wire_type::varint)
                        throw std::runtime_error("invalid protobuf JSON signed number field");
                    signed_value = wire::decode_zigzag(wire::read_varint(current, end));
                    type = decoded_type::signed_integer;
                    break;
                case 2:
                    if (field_type != wire::wire_type::varint)
                        throw std::runtime_error("invalid protobuf JSON unsigned number field");
                    unsigned_value = wire::read_varint(current, end);
                    type = decoded_type::unsigned_integer;
                    break;
                case 3:
                    if (field_type != wire::wire_type::fixed64)
                        throw std::runtime_error("invalid protobuf JSON floating number field");
                    floating_value = wire::read_double(current, end);
                    type = decoded_type::floating;
                    break;
                case 4:
                {
                    if (field_type != wire::wire_type::length_delimited)
                        throw std::runtime_error("invalid protobuf JSON floating lexical field");
                    const auto text = wire::read_length_delimited(current, end);
                    lexical_value.assign(text.data(), text.size());
                    type = decoded_type::floating_lexical;
                    break;
                }
                default:
                    wire::skip_field(current, end, field_type);
                    break;
                }
            }

            switch (type)
            {
            case decoded_type::signed_integer:
                *this = number(signed_value);
                break;
            case decoded_type::unsigned_integer:
                *this = number(unsigned_value);
                break;
            case decoded_type::floating:
                *this = number(floating_value);
                break;
            case decoded_type::floating_lexical:
                *this = number(std::move(lexical_value));
                break;
            }
        }

        inline void number::nanopb_serialise(std::vector<char>& buffer) const
        {
            protobuf_serialise(buffer);
        }

        inline void number::nanopb_deserialise(const std::vector<char>& buffer)
        {
            protobuf_deserialise(buffer);
        }

        inline void object::protobuf_serialise(std::vector<char>& buffer) const
        {
            namespace wire = detail::protobuf_wire;
            buffer.clear();
            switch (get_type())
            {
            case object::type::string_type:
                wire::append_string(buffer, 1, get<std::string>());
                break;
            case object::type::number_type:
            {
                std::vector<char> value;
                get<number>().protobuf_serialise(value);
                wire::append_length_delimited(buffer, 2, value);
                break;
            }
            case object::type::bool_type:
                wire::append_key(buffer, 3, wire::wire_type::varint);
                wire::append_varint(buffer, get<bool>() ? 1 : 0);
                break;
            case object::type::null_type:
                wire::append_key(buffer, 4, wire::wire_type::varint);
                wire::append_varint(buffer, 1);
                break;
            case object::type::array_type:
            {
                std::vector<char> value;
                as_array().protobuf_serialise(value);
                wire::append_length_delimited(buffer, 5, value);
                break;
            }
            case object::type::map_type:
            {
                std::vector<char> value;
                as_map().protobuf_serialise(value);
                wire::append_length_delimited(buffer, 6, value);
                break;
            }
            }
        }

        inline void object::protobuf_deserialise(const std::vector<char>& buffer)
        {
            namespace wire = detail::protobuf_wire;
            *this = nullptr;

            const char* current = wire::buffer_begin(buffer);
            const char* end = current + buffer.size();
            while (current != end)
            {
                uint32_t field_number = 0;
                wire::wire_type field_type{};
                wire::parse_key(current, end, field_number, field_type);

                switch (field_number)
                {
                case 1:
                {
                    if (field_type != wire::wire_type::length_delimited)
                        throw std::runtime_error("invalid protobuf JSON string field");
                    const auto text = wire::read_length_delimited(current, end);
                    *this = std::string(text.data(), text.size());
                    break;
                }
                case 2:
                {
                    if (field_type != wire::wire_type::length_delimited)
                        throw std::runtime_error("invalid protobuf JSON number field");
                    const auto bytes = wire::read_length_delimited(current, end);
                    std::vector<char> value_bytes(bytes.begin(), bytes.end());
                    number value;
                    value.protobuf_deserialise(value_bytes);
                    *this = value;
                    break;
                }
                case 3:
                    if (field_type != wire::wire_type::varint)
                        throw std::runtime_error("invalid protobuf JSON bool field");
                    *this = wire::read_varint(current, end) != 0;
                    break;
                case 4:
                    if (field_type != wire::wire_type::varint)
                        throw std::runtime_error("invalid protobuf JSON null field");
                    (void)wire::read_varint(current, end);
                    *this = nullptr;
                    break;
                case 5:
                {
                    if (field_type != wire::wire_type::length_delimited)
                        throw std::runtime_error("invalid protobuf JSON array field");
                    const auto bytes = wire::read_length_delimited(current, end);
                    std::vector<char> value_bytes(bytes.begin(), bytes.end());
                    array value;
                    value.protobuf_deserialise(value_bytes);
                    *this = std::move(value);
                    break;
                }
                case 6:
                {
                    if (field_type != wire::wire_type::length_delimited)
                        throw std::runtime_error("invalid protobuf JSON map field");
                    const auto bytes = wire::read_length_delimited(current, end);
                    std::vector<char> value_bytes(bytes.begin(), bytes.end());
                    map value;
                    value.protobuf_deserialise(value_bytes);
                    *this = std::move(value);
                    break;
                }
                default:
                    wire::skip_field(current, end, field_type);
                    break;
                }
            }
        }

        inline void object::nanopb_serialise(std::vector<char>& buffer) const
        {
            protobuf_serialise(buffer);
        }

        inline void object::nanopb_deserialise(const std::vector<char>& buffer)
        {
            protobuf_deserialise(buffer);
        }

        inline void map::protobuf_serialise(std::vector<char>& buffer) const
        {
            namespace wire = detail::protobuf_wire;
            buffer.clear();

            std::vector<const base::value_type*> entries;
            entries.reserve(size());
            for (const auto& entry : *this)
                entries.push_back(&entry);
            std::sort(
                entries.begin(), entries.end(), [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });

            for (const auto* entry : entries)
            {
                std::vector<char> entry_buffer;
                wire::append_string(entry_buffer, 1, entry->first);

                std::vector<char> value_buffer;
                entry->second.protobuf_serialise(value_buffer);
                wire::append_length_delimited(entry_buffer, 2, value_buffer);

                wire::append_length_delimited(buffer, 1, entry_buffer);
            }
        }

        inline void map::protobuf_deserialise(const std::vector<char>& buffer)
        {
            namespace wire = detail::protobuf_wire;
            clear();

            const char* current = wire::buffer_begin(buffer);
            const char* end = current + buffer.size();
            while (current != end)
            {
                uint32_t field_number = 0;
                wire::wire_type field_type{};
                wire::parse_key(current, end, field_number, field_type);
                if (field_number != 1 || field_type != wire::wire_type::length_delimited)
                {
                    wire::skip_field(current, end, field_type);
                    continue;
                }

                const auto entry = wire::read_length_delimited(current, end);
                const char* entry_current = wire::view_begin(entry);
                const char* entry_end = entry_current + entry.size();
                std::string key;
                object value;
                bool has_key = false;
                bool has_value = false;

                while (entry_current != entry_end)
                {
                    uint32_t entry_field_number = 0;
                    wire::wire_type entry_field_type{};
                    wire::parse_key(entry_current, entry_end, entry_field_number, entry_field_type);
                    switch (entry_field_number)
                    {
                    case 1:
                    {
                        if (entry_field_type != wire::wire_type::length_delimited)
                            throw std::runtime_error("invalid protobuf JSON map key field");
                        const auto text = wire::read_length_delimited(entry_current, entry_end);
                        key.assign(text.data(), text.size());
                        has_key = true;
                        break;
                    }
                    case 2:
                    {
                        if (entry_field_type != wire::wire_type::length_delimited)
                            throw std::runtime_error("invalid protobuf JSON map value field");
                        const auto bytes = wire::read_length_delimited(entry_current, entry_end);
                        std::vector<char> value_bytes(bytes.begin(), bytes.end());
                        value.protobuf_deserialise(value_bytes);
                        has_value = true;
                        break;
                    }
                    default:
                        wire::skip_field(entry_current, entry_end, entry_field_type);
                        break;
                    }
                }

                if (!has_key)
                    throw std::runtime_error("protobuf JSON map entry is missing key");
                (*this)[std::move(key)] = has_value ? std::move(value) : object(nullptr);
            }
        }

        inline void map::nanopb_serialise(std::vector<char>& buffer) const
        {
            protobuf_serialise(buffer);
        }

        inline void map::nanopb_deserialise(const std::vector<char>& buffer)
        {
            protobuf_deserialise(buffer);
        }

        inline void array::protobuf_serialise(std::vector<char>& buffer) const
        {
            namespace wire = detail::protobuf_wire;
            buffer.clear();
            for (const auto& value : *this)
            {
                std::vector<char> value_buffer;
                value.protobuf_serialise(value_buffer);
                wire::append_length_delimited(buffer, 1, value_buffer);
            }
        }

        inline void array::protobuf_deserialise(const std::vector<char>& buffer)
        {
            namespace wire = detail::protobuf_wire;
            clear();

            const char* current = wire::buffer_begin(buffer);
            const char* end = current + buffer.size();
            while (current != end)
            {
                uint32_t field_number = 0;
                wire::wire_type field_type{};
                wire::parse_key(current, end, field_number, field_type);
                if (field_number != 1 || field_type != wire::wire_type::length_delimited)
                {
                    wire::skip_field(current, end, field_type);
                    continue;
                }

                const auto bytes = wire::read_length_delimited(current, end);
                std::vector<char> value_bytes(bytes.begin(), bytes.end());
                object value;
                value.protobuf_deserialise(value_bytes);
                emplace_back(std::move(value));
            }
        }

        inline void array::nanopb_serialise(std::vector<char>& buffer) const
        {
            protobuf_serialise(buffer);
        }

        inline void array::nanopb_deserialise(const std::vector<char>& buffer)
        {
            protobuf_deserialise(buffer);
        }

#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
        inline bool number::canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const
        {
            if (!writer.append_u8(static_cast<uint8_t>(get_type())))
                return false;

            switch (get_type())
            {
            case number::type::signed_integer:
                return rpc::canonical_crypto_write(writer, as_int64());
            case number::type::unsigned_integer:
                return rpc::canonical_crypto_write(writer, as_uint64());
            case number::type::floating:
                return rpc::canonical_crypto_write(writer, as_double());
            }
            return false;
        }

        inline bool number::canonical_crypto_read_from(rpc::canonical_crypto_reader& reader)
        {
            uint8_t type = 0;
            if (!reader.read_u8(type))
                return false;

            switch (static_cast<number::type>(type))
            {
            case number::type::signed_integer:
            {
                int64_t value = 0;
                if (!rpc::canonical_crypto_read(reader, value))
                    return false;
                *this = number(value);
                return true;
            }
            case number::type::unsigned_integer:
            {
                uint64_t value = 0;
                if (!rpc::canonical_crypto_read(reader, value))
                    return false;
                *this = number(value);
                return true;
            }
            case number::type::floating:
            {
                double value = 0.0;
                if (!rpc::canonical_crypto_read(reader, value))
                    return false;
                *this = number(value);
                return true;
            }
            }
            return false;
        }

        inline bool object::canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const
        {
            if (!writer.append_u8(static_cast<uint8_t>(get_type())))
                return false;

            switch (get_type())
            {
            case object::type::string_type:
                return rpc::canonical_crypto_write(writer, get<std::string>());
            case object::type::number_type:
                return get<number>().canonical_crypto_write_to(writer);
            case object::type::bool_type:
                return rpc::canonical_crypto_write(writer, get<bool>());
            case object::type::null_type:
                return true;
            case object::type::array_type:
                return as_array().canonical_crypto_write_to(writer);
            case object::type::map_type:
                return as_map().canonical_crypto_write_to(writer);
            }
            return false;
        }

        inline bool object::canonical_crypto_read_from(rpc::canonical_crypto_reader& reader)
        {
            uint8_t type = 0;
            if (!reader.read_u8(type))
                return false;

            switch (static_cast<object::type>(type))
            {
            case object::type::string_type:
            {
                std::string value;
                if (!rpc::canonical_crypto_read(reader, value))
                    return false;
                *this = std::move(value);
                return true;
            }
            case object::type::number_type:
            {
                number value;
                if (!value.canonical_crypto_read_from(reader))
                    return false;
                *this = value;
                return true;
            }
            case object::type::bool_type:
            {
                bool value = false;
                if (!rpc::canonical_crypto_read(reader, value))
                    return false;
                *this = value;
                return true;
            }
            case object::type::null_type:
                *this = nullptr;
                return true;
            case object::type::array_type:
            {
                array value;
                if (!value.canonical_crypto_read_from(reader))
                    return false;
                *this = std::move(value);
                return true;
            }
            case object::type::map_type:
            {
                map value;
                if (!value.canonical_crypto_read_from(reader))
                    return false;
                *this = std::move(value);
                return true;
            }
            }
            return false;
        }

        inline bool map::canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const
        {
            if (size() > rpc::canonical_crypto_max_field_size || !writer.append_u64(static_cast<uint64_t>(size())))
                return false;

            std::vector<const base::value_type*> entries;
            entries.reserve(size());
            for (const auto& entry : *this)
                entries.push_back(&entry);
            std::sort(
                entries.begin(), entries.end(), [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });

            for (const auto* entry : entries)
            {
                if (!rpc::canonical_crypto_write(writer, entry->first) || !entry->second.canonical_crypto_write_to(writer))
                    return false;
            }
            return true;
        }

        inline bool map::canonical_crypto_read_from(rpc::canonical_crypto_reader& reader)
        {
            uint64_t size = 0;
            if (!reader.read_u64(size) || size > rpc::canonical_crypto_max_field_size
                || size > static_cast<uint64_t>(max_size()))
            {
                return false;
            }

            clear();
            for (uint64_t i = 0; i != size; ++i)
            {
                std::string key;
                object value;
                if (!rpc::canonical_crypto_read(reader, key) || !value.canonical_crypto_read_from(reader))
                    return false;

                auto inserted = emplace(std::move(key), std::move(value));
                if (!inserted.second)
                    return false;
            }
            return true;
        }

        inline bool array::canonical_crypto_write_to(rpc::canonical_crypto_writer& writer) const
        {
            if (size() > rpc::canonical_crypto_max_field_size || !writer.append_u64(static_cast<uint64_t>(size())))
                return false;

            for (const auto& value : *this)
            {
                if (!value.canonical_crypto_write_to(writer))
                    return false;
            }
            return true;
        }

        inline bool array::canonical_crypto_read_from(rpc::canonical_crypto_reader& reader)
        {
            uint64_t size = 0;
            if (!reader.read_u64(size) || size > rpc::canonical_crypto_max_field_size
                || size > static_cast<uint64_t>(max_size()))
            {
                return false;
            }

            clear();
            reserve(static_cast<size_t>(size));
            for (uint64_t i = 0; i != size; ++i)
            {
                object value;
                if (!value.canonical_crypto_read_from(reader))
                    return false;
                emplace_back(std::move(value));
            }
            return true;
        }
#endif
    } // namespace v1
} // namespace json

namespace rpc
{
    template<> class id<json::v1::number>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return 0x499940933C098C47; }
    };

    template<> class id<json::v1::object>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return 0xCCAC678202A8727D; }
    };

    template<> class id<json::v1::map>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return 0x972BC2102EAC5BEA; }
    };

    template<> class id<json::v1::array>
    {
    public:
        static constexpr uint64_t get(uint64_t) { return 0xA5C7AA0F92B7A23D; }
    };
} // namespace rpc
