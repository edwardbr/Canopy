/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <json/config_error.h>
#include <json/json_dom.h>

namespace json
{
    inline namespace v1
    {
        inline object parse_file(const std::filesystem::path& path)
        {
            std::ifstream input(path);
            if (!input)
                throw config_error("failed to open JSON config file: " + path.string());

            std::ostringstream buffer;
            buffer << input.rdbuf();
            return parse(buffer.str());
        }

        class config_view
        {
        public:
            config_view(
                const object& object,
                std::string path = "$")
                : object_(&object)
                , path_(std::move(path))
            {
            }

            [[nodiscard]] const object& value() const { return *object_; }
            [[nodiscard]] const std::string& path() const { return path_; }

            [[nodiscard]] const map& as_map() const
            {
                if (object_->get_type() != object::type::map_type)
                    throw config_error(path_ + " must be a JSON object");
                return object_->as_map();
            }

            [[nodiscard]] bool contains(std::string_view key) const
            {
                const auto& values = as_map();
                return values.find(std::string(key)) != values.end();
            }

            [[nodiscard]] config_view at(std::string_view key) const
            {
                const auto& values = as_map();
                const auto it = values.find(std::string(key));
                if (it == values.end())
                    throw config_error(path_ + "." + std::string(key) + " is required");
                return config_view(it->second, path_ + "." + std::string(key));
            }

            template<typename T> [[nodiscard]] T require() const
            {
                try
                {
                    return object_->get<T>();
                }
                catch (const std::exception& ex)
                {
                    throw config_error(path_ + " has the wrong type: " + ex.what());
                }
            }

            template<typename T> [[nodiscard]] std::optional<T> optional(std::string_view key) const
            {
                const auto& values = as_map();
                const auto it = values.find(std::string(key));
                if (it == values.end())
                    return std::nullopt;

                return config_view(it->second, path_ + "." + std::string(key)).require<T>();
            }

            template<typename T>
            void assign_optional(
                std::string_view key,
                T& destination) const
            {
                if (auto value = optional<T>(key))
                    destination = *value;
            }

            [[nodiscard]] std::chrono::milliseconds require_milliseconds() const
            {
                if (object_->get_type() == object::type::string_type)
                    return parse_duration(object_->get<std::string>(), path_);

                return std::chrono::milliseconds(require<int64_t>());
            }

            [[nodiscard]] std::optional<std::chrono::milliseconds> optional_milliseconds(std::string_view key) const
            {
                const auto& values = as_map();
                const auto it = values.find(std::string(key));
                if (it == values.end())
                    return std::nullopt;

                return config_view(it->second, path_ + "." + std::string(key)).require_milliseconds();
            }

        private:
            const object* object_;
            std::string path_;

            [[nodiscard]] static std::chrono::milliseconds parse_duration(
                const std::string& value,
                const std::string& path)
            {
                if (value.empty())
                    throw config_error(path + " duration string is empty");

                size_t suffix_pos = 0;
                while (suffix_pos < value.size()
                       && (std::isdigit(static_cast<unsigned char>(value[suffix_pos])) || value[suffix_pos] == '-'))
                    ++suffix_pos;

                if (suffix_pos == 0)
                    throw config_error(path + " duration must start with a number");

                const auto amount = std::stoll(value.substr(0, suffix_pos));
                const auto suffix = value.substr(suffix_pos);
                if (suffix.empty() || suffix == "ms")
                    return std::chrono::milliseconds(amount);
                if (suffix == "s")
                    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(amount));
                if (suffix == "m")
                    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(amount));

                throw config_error(path + " has unsupported duration suffix '" + suffix + "'");
            }
        };
    } // namespace v1
} // namespace json
