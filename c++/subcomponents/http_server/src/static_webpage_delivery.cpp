// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/static_webpage_delivery.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace canopy::http_server
{
    namespace
    {
        auto append_path_component(
            std::string& path,
            std::string_view component) -> void
        {
            if (!path.empty() && path.back() != '/')
            {
                path.push_back('/');
            }
            path.append(component);
        }

        auto request_to_static_file_path(
            std::string_view root_path,
            const request& request) -> std::optional<std::string>
        {
            if (root_path.empty())
            {
                return std::nullopt;
            }

            auto relative_path = request_path(request.url);
            if (relative_path.empty() || relative_path == "/")
            {
                relative_path = "/index.html";
            }

            std::string path{root_path};
            bool appended = false;
            size_t start = relative_path.starts_with("/") ? 1U : 0U;
            while (start <= relative_path.size())
            {
                const auto separator = relative_path.find('/', start);
                const auto end = separator == std::string::npos ? relative_path.size() : separator;
                const auto component = std::string_view(relative_path).substr(start, end - start);

                if (component == ".." || component.find('\\') != std::string_view::npos
                    || component.find('\0') != std::string_view::npos)
                {
                    return std::nullopt;
                }

                if (!component.empty() && component != ".")
                {
                    append_path_component(path, component);
                    appended = true;
                }

                if (separator == std::string::npos)
                {
                    break;
                }
                start = separator + 1;
            }

            if (!appended)
            {
                append_path_component(path, "index.html");
            }

            return path;
        }

        auto is_optional_browser_probe(std::string_view path) -> bool
        {
            return path == "/favicon.ico" || path == "/.well-known/appspecific/com.chrome.devtools.json";
        }
    } // namespace

    static_webpage_delivery::static_webpage_delivery(
        std::string root_path,
        async_file_reader file_reader)
        : root_path_(std::move(root_path))
        , file_reader_(std::move(file_reader))
    {
    }

    auto static_webpage_delivery::handle(const request& request) const -> CORO_TASK(std::optional<response>)
    {
        if (!file_reader_)
        {
            CO_RETURN std::optional<response>{make_text_response(500, "Static file reader unavailable")};
        }

        if (request.method != "GET")
        {
            CO_RETURN std::optional<response>{make_text_response(405, "Only GET method allowed for static files")};
        }

        auto candidate = request_to_static_file_path(root_path_, request);
        if (!candidate)
        {
            CO_RETURN std::optional<response>{make_text_response(403, "Forbidden")};
        }

        std::vector<uint8_t> data;
        const auto read_error = CO_AWAIT file_reader_(*candidate, data);
        if (read_error != rpc::error::OK())
        {
            const auto path = request_path(request.url);
            if (is_optional_browser_probe(path))
            {
                RPC_INFO(
                    "optional browser static request not found url={} file={} error={} ({}) - returning 404",
                    path,
                    *candidate,
                    read_error,
                    rpc::error::to_string(read_error));
            }
            else
            {
                RPC_WARNING(
                    "failed to read static file url={} file={} error={} ({}) - returning 404",
                    path,
                    *candidate,
                    read_error,
                    rpc::error::to_string(read_error));
            }
            CO_RETURN std::optional<response>{make_text_response(404, "Not Found")};
        }

        response output;
        output.status_code = 200;
        output.status_text = status_text(200);
        output.headers["Content-Type"] = content_type_for_path(*candidate);
        if (!data.empty())
        {
            output.body.assign(reinterpret_cast<const char*>(data.data()), data.size());
        }
        CO_RETURN std::optional<response>{std::move(output)};
    }

    auto static_webpage_delivery::content_type_for_path(std::string_view path) -> std::string
    {
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of("/\\");
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        {
            return "text/plain";
        }

        const auto extension = path.substr(dot);
        if (extension == ".html")
        {
            return "text/html";
        }
        if (extension == ".js")
        {
            return "application/javascript";
        }
        if (extension == ".css")
        {
            return "text/css";
        }
        if (extension == ".json")
        {
            return "application/json";
        }
        if (extension == ".png")
        {
            return "image/png";
        }
        if (extension == ".jpg" || extension == ".jpeg")
        {
            return "image/jpeg";
        }
        if (extension == ".gif")
        {
            return "image/gif";
        }
        return "text/plain";
    }
} // namespace canopy::http_server
