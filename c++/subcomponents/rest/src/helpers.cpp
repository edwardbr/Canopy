/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/helpers.h>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace canopy::rest
{
    std::string normalise_json(std::string_view text)
    {
        return json::v1::dump(json::v1::parse(text));
    }

    std::string encode_path_segment(std::string_view value)
    {
        std::ostringstream output;
        output << std::uppercase << std::hex;
        for (const unsigned char ch : value)
        {
            if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            {
                output << static_cast<char>(ch);
            }
            else
            {
                output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
            }
        }
        return output.str();
    }

    std::string append_path_segment(
        std::string_view base_path,
        std::string_view segment)
    {
        std::string path(base_path);
        if (path.empty() || path.back() != '/')
            path.push_back('/');
        path += encode_path_segment(segment);
        return path;
    }

    std::string required_string_field(
        std::string_view json_text,
        std::string_view field_name)
    {
        const auto request = json::v1::parse(json_text);
        const auto& fields = request.as_map();
        const auto item = fields.find(std::string(field_name));
        if (item == fields.end())
            throw std::runtime_error("required REST JSON field is missing: " + std::string(field_name));
        return item->second.get<std::string>();
    }
} // namespace canopy::rest
