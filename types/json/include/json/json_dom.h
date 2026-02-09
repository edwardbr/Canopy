// Copyright 2023 Secretarium Ltd <contact@secretarium.org>

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <cmath>

#include <yas/detail/io/serialization_exceptions.hpp>
#include <yas/detail/tools/json_tools.hpp>

#if defined(_MSC_VER) && defined(_IN_ENCLAVE) && !defined(_IN_ENCLAVE_MOCK)
#include <mpark/variant.hpp>
#define VARIANT_NS mpark
#else
#include <variant>
#define VARIANT_NS std
#endif

// these classes are to mimic json types and to read and write clean json as yas's default serialisation format is a bit
// yucky. json_object is a thin wrapper to a variant so to extract its value you need to use std::visit or sfinae
// json_array is a thin wrapper to std::vector<json_object>
// json_struct is a wrapper to std::unordered_map<std::string, json_object> with custom parsing

namespace json
{
    inline namespace v1
    {
        class object;
        class array;
        class map;

        class object
            : private VARIANT_NS::variant<std::string, double, bool, std::nullptr_t, std::unique_ptr<array>, std::unique_ptr<map>>
        {
        public:
            using container_type
                = VARIANT_NS::variant<std::string, double, bool, std::nullptr_t, std::unique_ptr<array>, std::unique_ptr<map>>;
            static constexpr size_t STRING_TYPE_INDEX = 0;
            static constexpr size_t DOUBLE_TYPE_INDEX = 1;
            static constexpr size_t BOOL_TYPE_INDEX = 2;
            static constexpr size_t NULL_TYPE_INDEX = 3;
            static constexpr size_t ARRAY_TYPE_INDEX = 4;
            static constexpr size_t MAP_TYPE_INDEX = 5;

            enum class type : uint8_t
            {
                string_type = STRING_TYPE_INDEX,
                double_type = DOUBLE_TYPE_INDEX,
                bool_type = BOOL_TYPE_INDEX,
                null_type = NULL_TYPE_INDEX,
                array_type = ARRAY_TYPE_INDEX,
                map_type = MAP_TYPE_INDEX
            };

            object();
            explicit object(const std::string& val);
            explicit object(std::string&& val);
            explicit object(const char* val);
            explicit object(int64_t val);
            explicit object(int32_t val);
            explicit object(int16_t val);
            explicit object(int8_t val);
            explicit object(uint64_t val);
            explicit object(uint32_t val);
            explicit object(uint16_t val);
            explicit object(uint8_t val);
            explicit object(double val);
            explicit object(bool val);
            explicit object(std::nullptr_t val);
            explicit object(const array& val);
            explicit object(array&& val);
            explicit object(std::unique_ptr<array>&& val);
            explicit object(const map& val);
            explicit object(map&& val);
            explicit object(std::unique_ptr<map>&& val);

            // you cannot make copy constructors explicit or RVO will break
            object(const object& other);
            explicit object(object&& other) = default;

            object& operator=(const std::string& val);
            object& operator=(std::string&& val);
            object& operator=(const char* val);
            object& operator=(int64_t val);
            object& operator=(int32_t val);
            object& operator=(int16_t val);
            object& operator=(int8_t val);
            object& operator=(uint64_t val);
            object& operator=(uint32_t val);
            object& operator=(uint16_t val);
            object& operator=(uint8_t val);
            object& operator=(double val);
            object& operator=(bool val);
            object& operator=(std::nullptr_t val);
            object& operator=(const object& val);
            object& operator=(object&& val);
            object& operator=(const array& val);
            object& operator=(std::unique_ptr<array>&& val);
            object& operator=(const map& val);
            object& operator=(std::unique_ptr<map>&& val);

            template<typename T> const T get() const;
            template<typename T> const T convert_to_int() const;

            type get_type() const { return static_cast<type>(index()); }

        private:
            friend inline bool operator!=(const object& lhs, const object& rhs);
        };

        // a thin wrapper of std::unordered_map<std::string, object> unfornately you cannot have a collection of a type
        // in the declaration of the type itself so map can be forward declared but not std::unordered_map<std::string,
        // object>
        class map : public std::unordered_map<std::string, object>
        {
        public:
            using base = std::unordered_map<std::string, object>;

            map() = default;
            map(const map& other) = default; // deep copy
            map(map&& other) = default;

            map& operator=(const map& other) = default; // deep copy
            map& operator=(map&& other) = default;      // move
        };

        // a thin wrapper of std::vector<object>, object> unfornately you cannot have a collection of a type in the declaration of the type itself
        //  so map can be forward declared but not std::vector<object>
        class array : public std::vector<object>
        {
        public:
            using base = std::vector<object>;
            array() = default;
            array(const array& other) = default; // deep copy
            array(array&& other) = default;
            array(std::initializer_list<object> init)
                : std::vector<object>(init)
            {
            }

            array& operator=(const array& other) = default; // deep copy
            array& operator=(array&& other) = default;      // move
        };

        // implementation
        inline object::object()
            : object::container_type(nullptr)
        {
        }
        inline object::object(const std::string& val)
            : object::container_type(val)
        {
        }
        inline object::object(std::string&& val)
            : object::container_type(std::move(val))
        {
        }
        inline object::object(const char* val)
            : object::container_type(std::string(val))
        {
        }
        inline object::object(int64_t val)
            : object::container_type(static_cast<double>(val))
        {
            if (static_cast<int64_t>(static_cast<double>(val)) != val)
                throw std::out_of_range("number too big");
        }
        inline object::object(int32_t val)
            : object::container_type(static_cast<double>(val))
        {
        }
        inline object::object(int16_t val)
            : object::container_type(static_cast<double>(val))
        {
        }
        inline object::object(int8_t val)
            : object::container_type(static_cast<double>(val))
        {
        }
        inline object::object(uint64_t val)
            : object::container_type(static_cast<double>(val))
        {
            if (static_cast<uint64_t>(static_cast<double>(val)) != val)
                throw std::out_of_range("number too big");
        }
        inline object::object(uint32_t val)
            : object::container_type(static_cast<double>(val))
        {
        }
        inline object::object(uint16_t val)
            : object::container_type(static_cast<double>(val))
        {
        }
        inline object::object(uint8_t val)
            : object::container_type(static_cast<double>(val))
        {
        }
        inline object::object(double val)
            : object::container_type(val)
        {
        }
        inline object::object(bool val)
            : object::container_type(val)
        {
        }
        inline object::object(std::nullptr_t val)
            : object::container_type(val)
        {
        }
        inline object::object(const array& val)
        {
            auto tmp = std::make_unique<array>(val);
            emplace<std::unique_ptr<array>>(std::move(tmp));
        }
        inline object::object(array&& val)
        {
            auto tmp = std::make_unique<array>(std::move(val));
            emplace<std::unique_ptr<array>>(std::move(tmp));
        }
        inline object::object(std::unique_ptr<array>&& val)
            : object::container_type(std::move(val))
        {
        }
        inline object::object(const map& val)
        {
            auto tmp = std::make_unique<map>(val);
            emplace<std::unique_ptr<map>>(std::move(tmp));
        }
        inline object::object(map&& val)
        {
            auto tmp = std::make_unique<map>(std::move(val));
            emplace<std::unique_ptr<map>>(std::move(tmp));
        }
        inline object::object(std::unique_ptr<map>&& val)
            : object::container_type(std::move(val))
        {
        }

        inline object::object(const object& other)
            : VARIANT_NS::variant<std::string, double, bool, std::nullptr_t, std::unique_ptr<array>, std::unique_ptr<map>>()
        {
            const container_type& ct_other = other;
            VARIANT_NS::visit(
                [this](auto&& other)
                {
                    using T = std::decay_t<decltype(other)>;
                    if constexpr (std::is_same<T, std::string>::value)
                        emplace<std::string>(other);
                    else if constexpr (std::is_same<T, double>::value)
                        emplace<double>(other);
                    else if constexpr (std::is_same<T, bool>::value)
                        emplace<bool>(other);
                    else if constexpr (std::is_same<T, std::nullptr_t>::value)
                        emplace<std::nullptr_t>(other);
                    else if constexpr (std::is_same<T, std::unique_ptr<array>>::value)
                    {
                        if (other)
                            emplace<std::unique_ptr<array>>(std::make_unique<array>(*other.get()));
                        else
                            __YAS_THROW_EXCEPTION(::yas::serialization_exception, "invalid array");
                    }
                    else if constexpr (std::is_same<T, std::unique_ptr<map>>::value)
                    {
                        if (other)
                            emplace<std::unique_ptr<map>>(std::make_unique<map>(*other.get()));
                        else
                            __YAS_THROW_EXCEPTION(::yas::serialization_exception, "invalid map");
                    }
                },
                ct_other);
        }

        inline object& object::operator=(const std::string& val)
        {
            emplace<std::string>(val);
            return *this;
        }
        inline object& object::operator=(std::string&& val)
        {
            emplace<std::string>(std::move(val));
            return *this;
        }
        inline object& object::operator=(const char* val)
        {
            emplace<std::string>(std::string(val));
            return *this;
        }
        inline object& object::operator=(int64_t val)
        {
            if (static_cast<int64_t>(static_cast<double>(val)) != val)
                throw std::out_of_range("number too big");

            emplace<double>(static_cast<double>(val));
            return *this;
        }
        inline object& object::operator=(int32_t val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(int16_t val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(int8_t val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(uint64_t val)
        {
            if (static_cast<uint64_t>(static_cast<double>(val)) != val)
                throw std::out_of_range("number too big");

            emplace<double>(static_cast<double>(val));
            return *this;
        }
        inline object& object::operator=(uint32_t val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(uint16_t val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(uint8_t val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(double val)
        {
            emplace<double>(val);
            return *this;
        }
        inline object& object::operator=(bool val)
        {
            emplace<bool>(val);
            return *this;
        }
        inline object& object::operator=(std::nullptr_t val)
        {
            emplace<std::nullptr_t>(val);
            return *this;
        }
        inline object& object::operator=(const object& val)
        {
            *this = object(val);
            return *this;
        }
        inline object& object::operator=(object&& val)
        {
            container_type* cont = this;
            (*cont) = std::move(val);
            return *this;
        }
        inline object& object::operator=(const array& val)
        {
            emplace<std::unique_ptr<array>>(std::make_unique<array>(val));
            return *this;
        }
        inline object& object::operator=(std::unique_ptr<array>&& val)
        {
            emplace<std::unique_ptr<array>>(std::move(val));
            return *this;
        }
        inline object& object::operator=(const map& val)
        {
            emplace<std::unique_ptr<map>>(std::make_unique<map>(val));
            return *this;
        }
        inline object& object::operator=(std::unique_ptr<map>&& val)
        {
            emplace<std::unique_ptr<map>>(std::move(val));
            return *this;
        }

        template<typename T> const T object::get() const
        {
            const container_type& cont = *this;
            if constexpr (std::is_same<T, std::string>::value)
                return VARIANT_NS::get<std::string>(cont);
            else if constexpr (std::is_same<T, bool>::value)
                return VARIANT_NS::get<bool>(cont);
            else if constexpr (std::is_floating_point<T>::value)
                return VARIANT_NS::get<double>(cont);
            else if constexpr (std::is_same<T, json::v1::array>::value)
            {
                auto& val = VARIANT_NS::get<std::unique_ptr<json::v1::array>>(cont);
                if (!val)
                    throw std::invalid_argument("invalid array");
                return *val;
            }
            else if constexpr (std::is_same<T, json::v1::map>::value)
            {
                auto& val = VARIANT_NS::get<std::unique_ptr<json::v1::map>>(cont);
                if (!val)
                    throw std::invalid_argument("invalid map");
                return *val;
            }
            else
            {
                // always false, but "depends" on the template parameter
                static_assert(!sizeof(T*));
            }
        }

        template<typename T> const T object::convert_to_int() const
        {
            static_assert(std::is_integral<T>::value);
            const container_type& cont = *this;
            auto val = VARIANT_NS::get<double>(cont);
            if (static_cast<double>(static_cast<T>(val)) != val)
                throw std::out_of_range("number too big for this type of parameter");
            return static_cast<T>(val);
        }

        inline bool operator!=(const object& lhs, const object& rhs);
        inline bool operator==(const object& lhs, const object& rhs);
        inline bool operator!=(const array& lhs, const array& rhs);
        inline bool operator==(const array& lhs, const array& rhs);
        inline bool operator!=(const map& lhs, const map& rhs);
        inline bool operator==(const map& lhs, const map& rhs);

        inline bool operator!=(const object& lhs, const object& rhs)
        {
            // match types
            if (lhs.get_type() != rhs.get_type())
                return true;

            if (lhs.get_type() == object::type::array_type) // array
            {
                const auto& larr = lhs.get<array>();
                const auto& rarr = rhs.get<array>();
                return larr != rarr;
            }
            else if (lhs.get_type() == object::type::map_type) // map
            {
                const auto& larr = lhs.get<map>();
                const auto& rarr = rhs.get<map>();
                return larr != rarr;
            }
            else
            { // use default matcher in VARIANT_NS::variant
                // use the base class for matching otherwise we will have a recursive death loop
                const object::container_type& l = lhs;
                const object::container_type& r = rhs;
                return l != r;
            }
        }

        inline bool operator==(const object& lhs, const object& rhs)
        {
            return !(lhs != rhs);
        }

        inline bool operator!=(const array& lhs, const array& rhs)
        {
            const array::base& l = lhs;
            const array::base& r = rhs;
            return l != r;
        }

        inline bool operator==(const array& lhs, const array& rhs)
        {
            const array::base& l = lhs;
            const array::base& r = rhs;
            return l == r;
        }

        inline bool operator!=(const map& lhs, const map& rhs)
        {
            const map::base& l = lhs;
            const map::base& r = rhs;
            return l != r;
        }
        inline bool operator==(const map& lhs, const map& rhs)
        {
            const map::base& l = lhs;
            const map::base& r = rhs;
            return l == r;
        }
    } // namespace v1
} // namespace json

namespace yas
{
    namespace detail
    {

        /***************************************************************************/

        // forward declare map serialisation
        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::map>
        {
            template<typename Archive> static Archive& save(Archive& ar, const json::v1::map& map);

            template<typename Archive> static Archive& load(Archive& ar, json::v1::map& map);
        };

        // forward declare array serialisation
        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::array>
        {
            template<typename Archive> static Archive& save(Archive& ar, const json::v1::array& map);

            template<typename Archive> static Archive& load(Archive& ar, json::v1::array& map);
        };

        // serialise objects
        template<std::size_t F>
        struct serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::object>
        {
            template<typename Archive> static Archive& save(Archive& ar, const json::v1::object& obj)
            {
                auto type = obj.get_type();
                if (F & yas::json)
                {
                    switch (type)
                    {
                    case json::v1::object::type::string_type:
                        ar & obj.get<std::string>();
                        break;
                    case json::v1::object::type::double_type:
                    {
                        double d = obj.get<double>();
                        if (std::fmod(d, 1) == 0) // whole numbers do need a decimal point
                        {
                            ar& static_cast<int64_t>(d);
                        }
                        else
                        {
                            ar & d;
                        }
                        break;
                    }
                    case json::v1::object::type::bool_type:
                        ar & obj.get<bool>();
                        break;
                    case json::v1::object::type::null_type:
                        ar.write("null", 4);
                        break;
                    case json::v1::object::type::array_type:
                        ar & obj.get<json::v1::array>();
                        break;
                    case json::v1::object::type::map_type:
                        ar & obj.get<json::v1::map>();
                        break;
                    }
                }
                else
                {
                    ar.write(static_cast<std::underlying_type_t<json::v1::object::type>>(type));
                    switch (type)
                    {
                    case json::v1::object::type::string_type:
                        ar & obj.get<std::string>();
                        break;
                    case json::v1::object::type::double_type:
                        ar & obj.get<double>();
                        break;
                    case json::v1::object::type::bool_type:
                        ar & obj.get<bool>();
                        break;
                    case json::v1::object::type::null_type:
                        break; // dont write anything
                    case json::v1::object::type::array_type:
                        ar & obj.get<json::v1::array>();
                        break;
                    case json::v1::object::type::map_type:
                        ar & obj.get<json::v1::map>();
                        break;
                    };
                }

                return ar;
            }

            // get the number type of the string
            static bool is_numeric(char ch)
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
                case '.':
                case 'e':
                case 'E':
                    return true;
                default:
                    return false;
                }
            }

            // copy numbers into a buffer returning the number type, if the first character is nan then return nan
            //  else it is an integer or floating point if there is rubbish further on expect yas to except anyway
            template<typename Archive> static bool scan_numeric(Archive& ar, char ch, std::string& str)
            {
                if (!is_numeric(ch))
                    return false;

                str += ch;

                if (ch == '-')
                {
                    ch = ar.peekch();
                    if (!is_numeric(ch))
                        return true;
                    str += ar.getch();
                }

                bool leading_with_0 = ch == '0';

                ch = ar.peekch();
                if (!is_numeric(ch))
                    return true;

                if (leading_with_0 && ch >= '0' && ch <= '9')
                    return false; // a quirk in the json standard no leading zero before other number digits

                str += ar.getch();

                while (true)
                {
                    ch = ar.peekch();
                    if (!is_numeric(ch))
                        return true;
                    str += ar.getch();
                }

                return true;
            }

            template<typename Archive> static Archive& load(Archive& ar, json::v1::object& obj)
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
                        obj = std::move(str);
                    }
                    else if (ch == 'n') // null
                    {
                        ar.ungetch(ch);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "null");
                        obj = nullptr;
                    }
                    else if (ch == 't') // true
                    {
                        ar.ungetch(ch);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "true");
                        obj = true;
                    }
                    else if (ch == 'f') // true
                    {
                        ar.ungetch(ch);
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "false");
                        obj = false;
                    }
                    else if (scan_numeric(ar, ch, str)) // number
                    {
                        double val = std::stod(str.c_str());
                        if (val == INFINITY || val == NAN)
                            __YAS_THROW_EXCEPTION(::yas::serialization_exception, "invalid number");
                        obj = val;
                    }
                    else if (ch == '[') // true
                    {
                        ar.ungetch(ch);
                        json::v1::array arr;
                        ar & arr;
                        obj = std::make_unique<json::v1::array>(std::move(arr));
                    }
                    else if (ch == '{') // true
                    {
                        ar.ungetch(ch);
                        auto str = std::make_unique<json::v1::map>();
                        ar&* str;
                        obj = std::move(str);
                    }
                    else
                    {
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, "unreachable");
                    }
                }
                else
                {
                    std::underlying_type_t<json::v1::object::type> idx = 0;
                    ar & idx;
                    auto type = static_cast<json::v1::object::type>(idx);
                    switch (type)
                    {
                    case json::v1::object::type::string_type:
                    {
                        std::string var;
                        ar & var;
                        obj = std::move(var);
                        break;
                    }
                    case json::v1::object::type::double_type:
                    {
                        double var = 0.0;
                        ar & var;
                        obj = var;
                        break;
                    }
                    case json::v1::object::type::bool_type:
                    {
                        bool var = false;
                        ar & var;
                        obj = var;
                        break;
                    }
                    case json::v1::object::type::null_type:
                    {
                        obj = nullptr;
                        break;
                    }
                    case json::v1::object::type::array_type:
                    {
                        json::v1::array var;
                        ar & var;
                        obj = std::move(var);
                        break;
                    }
                    case json::v1::object::type::map_type:
                    {
                        json::v1::map var;
                        ar & var;
                        obj = std::move(var);
                        break;
                    }

                    default:
                        __YAS_THROW_EXCEPTION(::yas::serialization_exception, "invalid bite stream");
                    };
                }
                return ar;
            }
        };

        template<std::size_t F>
        template<typename Archive>
        Archive& serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::map>::save(
            Archive& ar, const json::v1::map& map)
        {
            __YAS_CONSTEXPR_IF(F & yas::json)
            {
                if (map.empty())
                {
                    ar.write("{}", 2);
                    return ar;
                }

                ar.write("{", 1);
                auto it = map.begin();

                ar.write("\"", 1);
                save_string(ar, it->first.data(), it->first.size());
                ar.write("\"", 1);
                ar.write(":", 1);
                ar & it->second;

                for (++it; it != map.end(); ++it)
                {
                    ar.write(",", 1);
                    ar.write("\"", 1);
                    save_string(ar, it->first.data(), it->first.size());
                    ar.write("\"", 1);
                    ar.write(":", 1);
                    ar & it->second;
                }
                ar.write("}", 1);
            }
            else
            {
                ar.write_seq_size(map.size());
                for (const auto& it : map)
                {
                    ar & it.first & it.second;
                }
            }

            return ar;
        }

        template<std::size_t F>
        template<typename Archive>
        Archive& serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::map>::load(
            Archive& ar, json::v1::map& map)
        {
            __YAS_CONSTEXPR_IF(F & yas::json)
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

                // case for empty object
                const char ch = ar.peekch();
                if (ch == '}')
                {
                    ar.getch();

                    return ar;
                }

                __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                {
                    json_skipws(ar);
                }

                while (true)
                {
                    std::string name;
                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, "\"");
                    load_string(name, ar);
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

                    json::v1::object value(nullptr);
                    ar & value;
                    map.emplace(std::move(name), std::move(value));

                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    const char ch2 = ar.peekch();
                    if (ch2 == '}')
                    {
                        break;
                    }

                    __YAS_THROW_IF_BAD_JSON_CHARS(ar, ",");

                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }
                }

                __YAS_THROW_IF_BAD_JSON_CHARS(ar, "}");
            }
            else
            {
                auto size = ar.read_seq_size();
                for (; size; --size)
                {
                    std::string k;
                    json::v1::object v(nullptr);
                    ar & k & v;
                    map.emplace(std::move(k), std::move(v));
                }
            }

            return ar;
        }

        template<std::size_t F>
        template<typename Archive>
        Archive& serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::array>::save(
            Archive& ar, const json::v1::array& arr)
        {
            __YAS_CONSTEXPR_IF(F & yas::json)
            {
                ar.write("[", 1);

                if (!arr.empty())
                {
                    auto it = arr.begin();

                    ar&* it;
                    for (++it; it != arr.end(); ++it)
                    {
                        ar.write(",", 1);
                        ar&* it;
                    }
                }

                ar.write("]", 1);
            }
            else
            {
                const auto size = arr.size();
                ar.write_seq_size(size);
                if (size)
                {
                    for (const auto& ref : arr)
                    {
                        ar & ref;
                    }
                }
            }

            return ar;
        }

        template<std::size_t F>
        template<typename Archive>
        Archive& serializer<type_prop::not_a_fundamental, ser_case::use_internal_serializer, F, json::v1::array>::load(
            Archive& ar, json::v1::array& arr)
        {
            __YAS_CONSTEXPR_IF(F & yas::json)
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

                const char ch = ar.peekch();
                if (ch == ']')
                {
                    ar.getch();

                    return ar;
                }

                __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                {
                    json_skipws(ar);
                }

                while (true)
                {
                    json::v1::object v(nullptr);
                    ar & v;
                    arr.emplace_back(std::move(v));

                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }

                    const char ch2 = ar.peekch();
                    if (ch2 == ']')
                    {
                        break;
                    }
                    else
                    {
                        __YAS_THROW_IF_BAD_JSON_CHARS(ar, ",");
                    }

                    __YAS_CONSTEXPR_IF(!(F & yas::compacted))
                    {
                        json_skipws(ar);
                    }
                }

                json_skipws(ar);
                __YAS_THROW_IF_BAD_JSON_CHARS(ar, "]");
            }
            else
            {
                const auto size = ar.read_seq_size();
                if (size)
                {
                    arr.resize(size);

                    for (auto it = arr.begin(); it != arr.end(); ++it)
                    {
                        json::v1::object v(nullptr);
                        ar & v;
                        *it = std::move(v);
                    }
                }
            }

            return ar;
        }
        /***************************************************************************/

    } // namespace detail
} // namespace yas

namespace rpc
{
    template<> class id<json::v1::object>
    {
    public:
        static uint64_t get(uint64_t) { return 0xCCAC678202A8727D; }
    };

    template<> class id<json::v1::map>
    {
    public:
        static uint64_t get(uint64_t) { return 0x972BC2102EAC5BEA; }
    };

    template<> class id<json::v1::array>
    {
    public:
        static uint64_t get(uint64_t) { return 0xA5C7AA0F92B7A23D; }
    };
} // namespace rpc
