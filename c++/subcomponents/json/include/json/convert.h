/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <json/config_error.h>
#include <json/json_dom.h>
#include <rpc/internal/optional.h>
#include <rpc/internal/variant.h>

namespace json
{
    inline namespace v1
    {
        namespace convert
        {
            // Generated IDL converters live in their own namespace and use ADL
            // through this tag so primitives, optional/vector wrappers, and
            // user-supplied overloads compose without explicit specialisation.
            template<typename T> struct tag
            {
            };

            template<typename T> [[nodiscard]] T from_json_object(const json::v1::object& value);

            // ---- primitive readers ------------------------------------------------

            [[nodiscard]] inline bool from_json_object(
                tag<bool>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::bool_type)
                    throw config_error("expected JSON boolean");
                return value.get<bool>();
            }

            [[nodiscard]] inline std::string from_json_object(
                tag<std::string>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::string_type)
                    throw config_error("expected JSON string");
                return value.get<std::string>();
            }

            template<
                typename T,
                std::enable_if_t<
                    std::is_integral_v<T>
                        && !std::is_same_v<
                            T,
                            bool>,
                    int> = 0>
            [[nodiscard]] T from_json_object(
                tag<T>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::number_type)
                    throw config_error("expected JSON number");
                return value.get<number>().template as_integral<T>();
            }

            template<
                typename T,
                std::enable_if_t<
                    std::is_floating_point_v<T>,
                    int> = 0>
            [[nodiscard]] T from_json_object(
                tag<T>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::number_type)
                    throw config_error("expected JSON number");
                return static_cast<T>(value.get<number>().as_double());
            }

            // Raw escape hatch for fields whose concrete schema is decided at
            // runtime (the discriminator-driven late-binding case).
            [[nodiscard]] inline json::v1::object from_json_object(
                tag<json::v1::object>,
                const json::v1::object& value)
            {
                return value;
            }

            template<typename T>
            [[nodiscard]] rpc::optional<T> from_json_object(
                tag<rpc::optional<T>>,
                const json::v1::object& value)
            {
                if (value.get_type() == object::type::null_type)
                    return {};
                return rpc::optional<T>(from_json_object<T>(value));
            }

            template<
                typename T,
                std::size_t N>
            [[nodiscard]] std::array<
                T,
                N>
            from_json_object(
                tag<std::array<
                    T,
                    N>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::array_type)
                    throw config_error("expected JSON array");
                const auto& items = value.as_array();
                if (items.size() != N)
                    throw config_error("JSON array has the wrong fixed size");
                std::array<T, N> result{};
                for (std::size_t i = 0; i < N; ++i)
                    result[i] = from_json_object<T>(items[i]);
                return result;
            }

            namespace variant_detail
            {
                // Tag-keyed compile-time dispatch. The tag for each alternative
                // is supplied by rpc::variant_alternative_tag<T>::value so the
                // wire shape matches what rpc/internal/variant.h emits for YAS
                // JSON and what the schema generator emits for oneOf.
                template<
                    std::size_t Index,
                    typename Variant>
                [[nodiscard]] Variant load_by_tag_impl(
                    std::string_view requested,
                    const json::v1::object& /*value*/,
                    std::true_type /*end*/)
                {
                    throw config_error("rpc::variant JSON tag '" + std::string(requested) + "' is not a known alternative");
                }

                template<
                    std::size_t Index,
                    typename Variant>
                [[nodiscard]] Variant load_by_tag_impl(
                    std::string_view requested,
                    const json::v1::object& value,
                    std::false_type /*end*/)
                {
                    using alternative_type = rpc::variant_alternative_t<Index, Variant>;
                    if (requested == rpc::variant_alternative_tag<alternative_type>::value)
                        return Variant(from_json_object<alternative_type>(value));
                    using next_end = std::integral_constant<bool, (Index + 1U >= rpc::variant_size_v<Variant>)>;
                    return load_by_tag_impl<Index + 1U, Variant>(requested, value, next_end{});
                }
            } // namespace variant_detail

            template<typename... Ts>
            [[nodiscard]] rpc::variant<Ts...> from_json_object(
                tag<rpc::variant<Ts...>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::map_type)
                    throw config_error("expected JSON object for rpc::variant");
                const auto& map = value.as_map();
                if (map.size() != 1)
                    throw config_error("rpc::variant JSON object must have exactly one member");
                const auto& entry = *map.begin();

                using end = std::integral_constant<bool, (sizeof...(Ts) == 0)>;
                return variant_detail::load_by_tag_impl<0U, rpc::variant<Ts...>>(entry.first, entry.second, end{});
            }

            template<typename T>
            [[nodiscard]] std::vector<T> from_json_object(
                tag<std::vector<T>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::array_type)
                    throw config_error("expected JSON array");
                std::vector<T> result;
                const auto& items = value.as_array();
                result.reserve(items.size());
                for (const auto& item : items)
                    result.push_back(from_json_object<T>(item));
                return result;
            }

            template<typename T>
            [[nodiscard]] std::list<T> from_json_object(
                tag<std::list<T>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::array_type)
                    throw config_error("expected JSON array");
                std::list<T> result;
                for (const auto& item : value.as_array())
                    result.push_back(from_json_object<T>(item));
                return result;
            }

            template<typename T>
            [[nodiscard]] std::deque<T> from_json_object(
                tag<std::deque<T>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::array_type)
                    throw config_error("expected JSON array");
                std::deque<T> result;
                for (const auto& item : value.as_array())
                    result.push_back(from_json_object<T>(item));
                return result;
            }

            template<typename T>
            [[nodiscard]] std::set<T> from_json_object(
                tag<std::set<T>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::array_type)
                    throw config_error("expected JSON array");
                std::set<T> result;
                for (const auto& item : value.as_array())
                {
                    // Throw on duplicate input rather than silently dedupe.
                    // The matching schema also asserts uniqueItems: true.
                    if (!result.insert(from_json_object<T>(item)).second)
                        throw config_error("JSON array for std::set contains duplicate items");
                }
                return result;
            }

            template<typename T>
            [[nodiscard]] std::unordered_set<T> from_json_object(
                tag<std::unordered_set<T>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::array_type)
                    throw config_error("expected JSON array");
                std::unordered_set<T> result;
                for (const auto& item : value.as_array())
                {
                    if (!result.insert(from_json_object<T>(item)).second)
                        throw config_error("JSON array for std::unordered_set contains duplicate items");
                }
                return result;
            }

            // JSON only allows string keys, so map<K,V> is only supported when
            // K is std::string. Other key types remain on the unsupported list
            // in the generator.
            template<typename V>
            [[nodiscard]] std::map<
                std::string,
                V>
            from_json_object(
                tag<std::map<
                    std::string,
                    V>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::map_type)
                    throw config_error("expected JSON object");
                std::map<std::string, V> result;
                for (const auto& [key, sub] : value.as_map())
                    result.emplace(key, from_json_object<V>(sub));
                return result;
            }

            template<typename V>
            [[nodiscard]] std::unordered_map<
                std::string,
                V>
            from_json_object(
                tag<std::unordered_map<
                    std::string,
                    V>>,
                const json::v1::object& value)
            {
                if (value.get_type() != object::type::map_type)
                    throw config_error("expected JSON object");
                std::unordered_map<std::string, V> result;
                for (const auto& [key, sub] : value.as_map())
                    result.emplace(key, from_json_object<V>(sub));
                return result;
            }

            template<typename T> [[nodiscard]] T from_json_object(const json::v1::object& value)
            {
                return from_json_object(tag<T>{}, value);
            }

            // ---- primitive writers ------------------------------------------------

            [[nodiscard]] inline json::v1::object to_json_object(bool value)
            {
                return json::v1::object(value);
            }

            [[nodiscard]] inline json::v1::object to_json_object(const std::string& value)
            {
                return json::v1::object(value);
            }

            [[nodiscard]] inline json::v1::object to_json_object(const char* value)
            {
                return json::v1::object(std::string(value));
            }

            template<
                typename T,
                std::enable_if_t<
                    std::is_integral_v<T>
                        && !std::is_same_v<
                            T,
                            bool>
                        && std::is_signed_v<T>,
                    int> = 0>
            [[nodiscard]] json::v1::object to_json_object(T value)
            {
                return json::v1::object(static_cast<int64_t>(value));
            }

            template<
                typename T,
                std::enable_if_t<
                    std::is_integral_v<T>
                        && !std::is_same_v<
                            T,
                            bool>
                        && std::is_unsigned_v<T>,
                    int> = 0>
            [[nodiscard]] json::v1::object to_json_object(T value)
            {
                return json::v1::object(static_cast<uint64_t>(value));
            }

            template<
                typename T,
                std::enable_if_t<
                    std::is_floating_point_v<T>,
                    int> = 0>
            [[nodiscard]] json::v1::object to_json_object(T value)
            {
                return json::v1::object(static_cast<double>(value));
            }

            [[nodiscard]] inline json::v1::object to_json_object(const json::v1::object& value)
            {
                return value;
            }

            template<typename T> [[nodiscard]] json::v1::object to_json_object(const rpc::optional<T>& value)
            {
                if (!value.has_value())
                    return json::v1::object(nullptr);
                return to_json_object(*value);
            }

            template<typename T> [[nodiscard]] json::v1::object to_json_object(const std::vector<T>& value)
            {
                json::v1::array result;
                result.reserve(value.size());
                for (const auto& item : value)
                    result.emplace_back(to_json_object(item));
                return json::v1::object(std::move(result));
            }

            template<typename T> [[nodiscard]] json::v1::object to_json_object(const std::list<T>& value)
            {
                json::v1::array result;
                for (const auto& item : value)
                    result.emplace_back(to_json_object(item));
                return json::v1::object(std::move(result));
            }

            template<typename T> [[nodiscard]] json::v1::object to_json_object(const std::deque<T>& value)
            {
                json::v1::array result;
                for (const auto& item : value)
                    result.emplace_back(to_json_object(item));
                return json::v1::object(std::move(result));
            }

            template<typename T> [[nodiscard]] json::v1::object to_json_object(const std::set<T>& value)
            {
                json::v1::array result;
                for (const auto& item : value)
                    result.emplace_back(to_json_object(item));
                return json::v1::object(std::move(result));
            }

            template<typename T> [[nodiscard]] json::v1::object to_json_object(const std::unordered_set<T>& value)
            {
                json::v1::array result;
                for (const auto& item : value)
                    result.emplace_back(to_json_object(item));
                return json::v1::object(std::move(result));
            }

            template<typename V>
            [[nodiscard]] json::v1::object to_json_object(
                const std::map<
                    std::string,
                    V>& value)
            {
                json::v1::map result;
                for (const auto& [key, sub] : value)
                    result.emplace(key, to_json_object(sub));
                return json::v1::object(std::move(result));
            }

            template<typename V>
            [[nodiscard]] json::v1::object to_json_object(
                const std::unordered_map<
                    std::string,
                    V>& value)
            {
                json::v1::map result;
                for (const auto& [key, sub] : value)
                    result.emplace(key, to_json_object(sub));
                return json::v1::object(std::move(result));
            }

            template<
                typename T,
                std::size_t N>
            [[nodiscard]] json::v1::object to_json_object(
                const std::array<
                    T,
                    N>& value)
            {
                json::v1::array result;
                result.reserve(N);
                for (const auto& item : value)
                    result.emplace_back(to_json_object(item));
                return json::v1::object(std::move(result));
            }

            template<typename... Ts> [[nodiscard]] json::v1::object to_json_object(const rpc::variant<Ts...>& value)
            {
                if (value.valueless_by_exception())
                    throw config_error("cannot serialise valueless rpc::variant");

                json::v1::map result;
                rpc::visit(
                    [&result](const auto& alternative)
                    {
                        using alternative_type = std::decay_t<decltype(alternative)>;
                        result.emplace(rpc::variant_alternative_tag<alternative_type>::value, to_json_object(alternative));
                    },
                    value);
                return json::v1::object(std::move(result));
            }
        } // namespace convert
    } // namespace v1
} // namespace json
