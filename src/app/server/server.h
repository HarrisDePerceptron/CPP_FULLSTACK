#pragma once

#include "app/config.h"
#include "framework/db/database_backend.h"
#include "framework/http/server.h"
#include "framework/http/server_runtime.h"
#include "framework/support/base_config.h"
#include "routes.h"
#include "services.h"

#include <drogon/HttpAppFramework.h>

#include <filesystem>

namespace APP_NAMESPACE {

inline int serve(const std::filesystem::path& root) {
    const auto config = load_config<Config>(root);
    return run_http_server(drogon::app(), root, config.server,
                           [&config](ServerRuntime& runtime) -> drogon::Task<> {
                               auto database = co_await open_sqlite_database(
                                   runtime.config().database_path);
                               auto services = co_await initialize_services(
                                   database, config, runtime);
                               register_app_routes(runtime, services);
                           });
}

} // namespace APP_NAMESPACE
