#pragma once

#include "app_namespace.h"
#include "app/config.h"
#include "framework/http/server_hooks.h"
#include "framework/storage/uploads.h"
#include "framework/support/base_config.h"
#include "routes.h"
#include "services.h"

#include <drogon/HttpAppFramework.h>
#include <sodium.h>
#include <trantor/net/EventLoop.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace server_detail {

inline constexpr auto public_url_prefix = "/public/";

inline void prepare_server_files(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / ".local" / "empty");
    std::filesystem::create_directories(root / "public" / "uploads");
}

inline void configure_http_server(drogon::HttpAppFramework& server, const std::filesystem::path& root,
                                  const Config& config) {
    constexpr std::size_t max_request_bytes = max_upload_bytes + 64 * 1024;
    const auto state = root / ".local";
    server.addListener(config.bind_address, static_cast<std::uint16_t>(config.port))
        .setDocumentRoot((state / "empty").string())
        .setUploadPath((state / "request-cache").string())
        .setImplicitPageEnable(false)
        .setClientMaxBodySize(max_request_bytes)
        .setMaxConnectionNum(config.max_connections)
        .setFileTypes({"html", "css", "js", "png", "jpg", "jpeg", "gif", "webp", "pdf", "svg"})
        .addALocation(public_url_prefix, "", (root / "public").string(), true, false, false);
}

inline void register_listening_message(drogon::HttpAppFramework& server, const Config& config, Logger log) {
    server.registerBeginningAdvice([log, address = config.bind_address, port = config.port] {
        log.write(LogLevel::info, "Listening on http://{}:{}", address, port);
        std::fflush(stdout);
    });
}

inline void register_shutdown(drogon::HttpAppFramework& server, const std::shared_ptr<Services>& services) {
    server.setTermSignalHandler([services] { services->stopping.test_and_set(); })
        .setIntSignalHandler([services] { services->stopping.test_and_set(); });
    server.registerBeginningAdvice([&server, services] {
        server.getLoop()->runEvery(0.1, [&server, services] {
            if (services->stopping.test()) {
                services->workers.stop();
                server.quit();
            }
        });
    });
}

}

inline int serve(const std::filesystem::path& root) {
    if (sodium_init() < 0) throw std::runtime_error("could not initialize libsodium");
    const auto config = load_config<Config>(root);
    const auto log_level = parse_log_level(config.log_level);
    server_detail::prepare_server_files(root);
    const auto services = initialize_services(root, config, log_level);

    auto& server = drogon::app();
    server_detail::configure_http_server(server, root, config);
    register_access_logging(server, log_level, services->log, server_detail::public_url_prefix);
    register_exception_handler(server, services->log);
    register_app_routes(server, services);
    server_detail::register_listening_message(server, config, services->log);
    server_detail::register_shutdown(server, services);
    server.run();
    return 0;
}

}
