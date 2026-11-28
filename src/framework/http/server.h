#pragma once

#include "app_namespace.h"
#include "framework/db/database_backend.h"
#include "framework/storage/uploads.h"
#include "framework/support/log.h"

#include <drogon/utils/coroutine.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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
    std::string uploads_url_prefix;
    std::size_t max_upload_bytes;
    std::size_t max_request_bytes;
};

namespace detail {

using MountRoutes =
    std::function<void(drogon::HttpAppFramework&, const ServerRuntime&)>;
using ServerSetup =
    std::function<drogon::Task<MountRoutes>(DatabaseBackendPtr)>;

int run_http_server(drogon::HttpAppFramework& server,
                    const std::filesystem::path& root,
                    const ServerConfig& config, const ServerSetup& setup);

}

template <typename Initialize, typename RegisterRoutes>
int run_http_server(drogon::HttpAppFramework& server,
                    const std::filesystem::path& root,
                    const ServerConfig& config, Initialize initialize,
                    RegisterRoutes register_routes) {
    return detail::run_http_server(
        server, root, config,
        [initialize = std::move(initialize),
         register_routes = std::move(register_routes)](
            DatabaseBackendPtr database) -> drogon::Task<detail::MountRoutes> {
            auto services =
                co_await std::invoke(initialize, std::move(database));
            co_return [services = std::move(services),
                       register_routes](drogon::HttpAppFramework& app,
                                        const ServerRuntime& runtime) {
                std::invoke(register_routes, app, services, runtime);
            };
        });
}

}
