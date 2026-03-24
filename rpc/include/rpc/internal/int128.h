#include <string_view>
#include <string>

// Disable pedantic warnings for YAS library headers
YAS_WARNINGS_PUSH

#include <yas/count_streams.hpp>
#include <yas/serialize.hpp>
#include <yas/std_types.hpp>
#include <yas/object.hpp>

YAS_WARNINGS_POP

namespace rpc
{
    inline std::string uint128_to_string(unsigned __int128 v)
    {
        if (v == 0)
            return "0";
        char buf[40];
        char* p = buf + sizeof(buf);
        while (v)
        {
            *--p = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        }
        return std::string(p, buf + sizeof(buf));
    }

    inline std::string int128_to_string(__int128 v)
    {
        if (v < 0)
        {
            const auto magnitude = static_cast<unsigned __int128>(-(v + 1)) + 1;
            return "-" + uint128_to_string(magnitude);
        }
        return uint128_to_string(static_cast<unsigned __int128>(v));
    }

    inline unsigned __int128 string_to_uint128(std::string_view s)
    {
        unsigned __int128 result = 0;
        for (char c : s)
            result = result * 10 + static_cast<unsigned>(c - '0');
        return result;
    }

    inline __int128 string_to_int128(std::string_view s)
    {
        if (!s.empty() && s[0] == '-')
            return -static_cast<__int128>(string_to_uint128(s.substr(1)));
        return static_cast<__int128>(string_to_uint128(s));
    }
}

namespace yas
{
    namespace detail
    {
        // Specialise serialization_method to bypass the has_const_memfn_serializer
        // check, which tries to inherit from T and fails for scalar types like __int128.
        template<typename Ar> struct serialization_method<__int128, Ar>
        {
            static constexpr ser_case value = ser_case::use_internal_serializer;
        };

        template<typename Ar> struct serialization_method<unsigned __int128, Ar>
        {
            static constexpr ser_case value = ser_case::use_internal_serializer;
        };

        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, __int128>
        {
            template<typename Archive>
            static Archive& save(
                Archive& ar,
                const __int128& obj)
            {
                if (F & yas::json)
                {
                    auto tmp = rpc::int128_to_string(obj);
                    ar & tmp;
                }
                else
                {
                    auto* parts = reinterpret_cast<const uint64_t*>(&obj);

                    ar& parts[0];
                    ar& parts[1];
                }

                return ar;
            }

            template<typename Archive>
            static Archive& load(
                Archive& ar,
                __int128& obj)
            {
                if (F & yas::json)
                {
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }
                    std::string str;
                    char ch = ar.getch();
                    if (ch == '\"') // string
                    {
                        load_string(str, ar);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "\"");
                        obj = rpc::string_to_int128(str);
                    }

                    else
                    {
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "unreachable");
                    }
                }
                else
                {
                    auto* parts = reinterpret_cast<uint64_t*>(&obj);

                    ar& parts[0];
                    ar& parts[1];
                }
                return ar;
            }
        };

        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, unsigned __int128>
        {
            template<typename Archive>
            static Archive& save(
                Archive& ar,
                const unsigned __int128& obj)
            {
                if (F & yas::json)
                {
                    auto tmp = rpc::uint128_to_string(obj);
                    ar & tmp;
                }
                else
                {
                    auto* parts = reinterpret_cast<const uint64_t*>(&obj);

                    ar& parts[0];
                    ar& parts[1];
                }

                return ar;
            }

            template<typename Archive>
            static Archive& load(
                Archive& ar,
                unsigned __int128& obj)
            {
                if (F & yas::json)
                {
                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }
                    std::string str;
                    char ch = ar.getch();
                    if (ch == '\"') // string
                    {
                        load_string(str, ar);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "\"");
                        obj = rpc::string_to_uint128(str);
                    }

                    else
                    {
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "unreachable");
                    }
                }
                else
                {
                    auto* parts = reinterpret_cast<uint64_t*>(&obj);

                    ar& parts[0];
                    ar& parts[1];
                }
                return ar;
            }
        };
    }
}
