#pragma once

#include "app_namespace.h"
#include "framework/storage/uploads.h"

#include <drogon/utils/coroutine.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace drogon {
class HttpAppFramework;
}

namespace APP_NAMESPACE {

class ServerRuntime;

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    int port = 8080;
    std::size_t max_connections = 64;
    std::string log_level = "info";
    int jwt_lifetime_seconds = 1800;
    std::string state_directory = ".local";
    std::string database_filename = "users.sqlite3";
    std::string public_directory = "public";
    std::string public_url_prefix = "/public/";
    std::string uploads_subdirectory = "uploads";
    std::string request_cache_subdirectory = "request-cache";
    std::size_t max_upload_bytes = APP_NAMESPACE::max_upload_bytes;

    void validate() const;
};

struct ResolvedServerConfig {
    std::filesystem::path database_path;
    std::filesystem::path document_root;
    std::filesystem::path request_cache_directory;
    std::filesystem::path public_directory;
    std::filesystem::path uploads_directory;
    std::string public_url_prefix;
    std::size_t max_upload_bytes;
    std::size_t max_request_bytes;
};

using ServerSetup = std::function<drogon::Task<>(ServerRuntime&)>;

int run_http_server(drogon::HttpAppFramework& server,
                    const std::filesystem::path& root,
                    const ServerConfig& config, const ServerSetup& setup);

}
