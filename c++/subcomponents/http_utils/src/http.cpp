/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/http_utils/http.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <system_error>

namespace canopy::http_utils
{
    namespace
    {
        [[nodiscard]] char ascii_lower(char value)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        }

        [[nodiscard]] int hex_value(char ch)
        {
            if (ch >= '0' && ch <= '9')
                return ch - '0';
            if (ch >= 'a' && ch <= 'f')
                return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F')
                return ch - 'A' + 10;
            return -1;
        }
    }

    std::string ascii_lower_copy(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    std::string_view trim_ascii(std::string_view input)
    {
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
            input.remove_prefix(1);
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
            input.remove_suffix(1);
        return input;
    }

    std::string trim_ascii_copy(std::string_view input)
    {
        return std::string(trim_ascii(input));
    }

    bool ascii_iequals(
        std::string_view lhs,
        std::string_view rhs)
    {
        return lhs.size() == rhs.size()
               && std::equal(
                   lhs.begin(),
                   lhs.end(),
                   rhs.begin(),
                   [](char left, char right) { return ascii_lower(left) == ascii_lower(right); });
    }

    bool is_http_token_char(unsigned char ch)
    {
        if (std::isalnum(ch))
            return true;

        switch (ch)
        {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
        }
    }

    bool is_http_token(std::string_view value)
    {
        return !value.empty()
               && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return is_http_token_char(ch); });
    }

    bool has_header_value_control(std::string_view value)
    {
        return std::any_of(
            value.begin(), value.end(), [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == '\0'; });
    }

    bool is_status_text_safe(std::string_view value)
    {
        return !has_header_value_control(value);
    }

    bool is_header_safe(
        std::string_view name,
        std::string_view value)
    {
        return is_http_token(name) && !has_header_value_control(value);
    }

    bool header_contains_token(
        std::string_view value,
        std::string_view token)
    {
        while (!value.empty())
        {
            const auto comma = value.find(',');
            auto part = trim_ascii(value.substr(0, comma));
            if (ascii_iequals(part, token))
                return true;
            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }
        return false;
    }

    size_t parse_content_length(std::string_view value)
    {
        value = trim_ascii(value);
        size_t result = 0;
        const auto parse_result = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parse_result.ec != std::errc{} || parse_result.ptr != value.data() + value.size())
            throw std::runtime_error("invalid HTTP Content-Length");
        return result;
    }

    std::string decode_chunked_body(std::string_view body)
    {
        std::string decoded;
        size_t offset = 0;
        while (true)
        {
            const auto line_end = body.find("\r\n", offset);
            if (line_end == std::string_view::npos)
                throw std::runtime_error("invalid chunked HTTP response");

            auto size_text = body.substr(offset, line_end - offset);
            const auto extension = size_text.find(';');
            if (extension != std::string_view::npos)
                size_text = size_text.substr(0, extension);

            size_t chunk_size = 0;
            const auto parse_result
                = std::from_chars(size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
            if (parse_result.ec != std::errc{} || parse_result.ptr != size_text.data() + size_text.size())
                throw std::runtime_error("invalid chunk size in HTTP response");

            offset = line_end + 2;
            if (chunk_size == 0)
            {
                while (true)
                {
                    const auto trailer_line_end = body.find("\r\n", offset);
                    if (trailer_line_end == std::string_view::npos)
                        throw std::runtime_error("truncated chunked HTTP response");
                    if (trailer_line_end == offset)
                        return decoded;
                    offset = trailer_line_end + 2;
                }
            }
            if (chunk_size > body.size() - offset)
                throw std::runtime_error("truncated chunked HTTP response");

            decoded.append(body.data() + offset, chunk_size);
            offset += chunk_size;
            if (body.size() - offset < 2 || body.compare(offset, 2, "\r\n") != 0)
                throw std::runtime_error("invalid chunk delimiter in HTTP response");
            offset += 2;
        }
        return decoded;
    }

    std::string request_path(
        std::string_view target,
        std::string_view empty_path)
    {
        const auto query = target.find_first_of("?#");
        auto path = std::string(target.substr(0, query));
        return path.empty() ? std::string(empty_path) : path;
    }

    std::string percent_decode(
        std::string_view value,
        bool plus_is_space)
    {
        std::string output;
        output.reserve(value.size());
        for (size_t index = 0; index < value.size(); ++index)
        {
            const auto ch = value[index];
            if (plus_is_space && ch == '+')
            {
                output.push_back(' ');
            }
            else if (ch == '%' && index + 2 < value.size())
            {
                const auto high = hex_value(value[index + 1]);
                const auto low = hex_value(value[index + 2]);
                if (high >= 0 && low >= 0)
                {
                    output.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                }
                else
                {
                    output.push_back(ch);
                }
            }
            else
            {
                output.push_back(ch);
            }
        }
        return output;
    }

    std::optional<std::string> percent_decode_strict(
        std::string_view value,
        bool plus_is_space)
    {
        std::string output;
        output.reserve(value.size());
        for (size_t index = 0; index < value.size(); ++index)
        {
            const auto ch = value[index];
            if (plus_is_space && ch == '+')
            {
                output.push_back(' ');
                continue;
            }
            if (ch != '%')
            {
                output.push_back(ch);
                continue;
            }
            if (index + 2 >= value.size())
                return std::nullopt;

            const auto high = hex_value(value[index + 1]);
            const auto low = hex_value(value[index + 2]);
            if (high < 0 || low < 0)
                return std::nullopt;

            output.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        }
        return output;
    }
}
