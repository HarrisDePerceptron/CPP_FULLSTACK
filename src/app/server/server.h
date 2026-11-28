#pragma once

#include "app/config.h"
#include "framework/http/server.h"
#include "framework/support/base_config.h"
#include "routes.h"
#include "services.h"

#include <drogon/HttpAppFramework.h>

#include <filesystem>

namespace APP_NAMESPACE {

inline int serve(const std::filesystem::path& root) {
    const auto config = load_config<Config>(root);

    return run_http_server(
        drogon::app(), root, config.server,
        [&config](const DatabaseBackendPtr& database) {
            return initialize_services(database, config);
        },
        register_app_routes);
}

} // namespace APP_NAMESPACE
