// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace websocket_demo
{
    namespace v1
    {
        std::string http_client_connection::get_content_type(const std::string& path)
        {
            if (path.ends_with(".html"))
                return "text/html";
            if (path.ends_with(".js"))
                return "application/javascript";
            if (path.ends_with(".css"))
                return "text/css";
            if (path.ends_with(".json"))
                return "application/json";
            if (path.ends_with(".png"))
                return "image/png";
            if (path.ends_with(".jpg") || path.ends_with(".jpeg"))
                return "image/jpeg";
            if (path.ends_with(".gif"))
                return "image/gif";
            return "text/plain";
        }

        std::string http_client_connection::serve_file(const std::string& path)
        {
            namespace fs = std::filesystem;

            if (path.find("..") != std::string::npos)
            {
                std::map<std::string, std::string> headers = {{"Content-Type", "text/plain"}, {"Connection", "close"}};
                return build_http_response(403, "Forbidden", headers, "Forbidden");
            }

            fs::path www_root = fs::path(__FILE__).parent_path() / "www";
            fs::path file_path = www_root / path.substr(1);

            if (!fs::exists(file_path) || !fs::is_regular_file(file_path))
            {
                std::map<std::string, std::string> headers = {{"Content-Type", "text/plain"}, {"Connection", "close"}};
                return build_http_response(404, "Not Found", headers, "Not Found");
            }

            std::ifstream file(file_path, std::ios::binary);
            if (!file.is_open())
            {
                std::map<std::string, std::string> headers = {{"Content-Type", "text/plain"}, {"Connection", "close"}};
                return build_http_response(500, "Internal Server Error", headers, "Internal Server Error");
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            std::string content_type = get_content_type(path);
            std::map<std::string, std::string> headers = {{"Content-Type", content_type}, {"Connection", "keep-alive"}};

            return build_http_response(200, "OK", headers, content);
        }

        std::string http_client_connection::handle_browser_request(const std::string& method, const std::string& path)
        {
            if (method != "GET")
            {
                return create_error_response(405, "Only GET method allowed for static files");
            }

            std::string file_path = path;
            if (file_path == "/" || file_path.empty())
            {
                file_path = "/index.html";
            }

            return serve_file(file_path);
        }
    }
}
