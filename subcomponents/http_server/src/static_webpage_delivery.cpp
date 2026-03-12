// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/static_webpage_delivery.h>

#include <fstream>
#include <sstream>

namespace canopy::http_server
{
    static_webpage_delivery::static_webpage_delivery(std::filesystem::path root_path)
        : root_path_(std::move(root_path))
    {
    }

    auto static_webpage_delivery::handle(const request& request) const -> std::optional<response>
    {
        namespace fs = std::filesystem;

        if (request.method != "GET")
        {
            return make_text_response(405, "Only GET method allowed for static files");
        }

        auto relative_path = request_path(request.url);
        if (relative_path.empty() || relative_path == "/")
        {
            relative_path = "/index.html";
        }

        fs::path relative(relative_path.starts_with("/") ? relative_path.substr(1) : relative_path);
        fs::path root = fs::weakly_canonical(root_path_);
        fs::path candidate = fs::weakly_canonical(root / relative);

        auto root_string = root.generic_string();
        auto candidate_string = candidate.generic_string();
        if (candidate != root && candidate_string.rfind(root_string + "/", 0) != 0)
        {
            return make_text_response(403, "Forbidden");
        }

        if (!fs::exists(candidate) || !fs::is_regular_file(candidate))
        {
            return make_text_response(404, "Not Found");
        }

        std::ifstream file(candidate, std::ios::binary);
        if (!file.is_open())
        {
            return make_text_response(500, "Internal Server Error");
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        response output;
        output.status_code = 200;
        output.status_text = status_text(200);
        output.headers["Content-Type"] = get_content_type(candidate);
        output.body = buffer.str();
        return output;
    }

    auto static_webpage_delivery::get_content_type(const std::filesystem::path& path) -> std::string
    {
        const auto extension = path.extension().string();
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
