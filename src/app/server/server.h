#pragma once

#include "app/config.h"
#include "framework/http/server.h"
#include "framework/support/base_config.h"
#include "routes.h"
#include "services.h"

#include <drogon/HttpAppFramework.h>

#include <filesystem>
#include <memory>

namespace APP_NAMESPACE {

inline int serve(const std::filesystem::path& root) {
    const auto config = load_config<Config>(root);
    return run_http_server(drogon::app(), root, config.server,
        [&config](auto& server, const ResolvedServerConfig& resolved, const Logger& log) {
            const auto services = initialize_services(resolved, config, log);
            register_app_routes(server, services, resolved);
            return std::shared_ptr<Workers>(services, &services->workers);
        });
}

} // namespace APP_NAMESPACE
