#include "app_namespace.h"
#include "server.h"

#include "app/config.h"
#include "framework/auth/jwt_middleware.h"
#include "framework/http/json_response.h"
#include "framework/support/base_config.h"
#include "routes.h"
#include "services.h"

#include <drogon/drogon.h>
#include <sodium.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Logger.h>

#include <cstdio>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;
namespace fs = std::filesystem;

constexpr std::size_t max_request_bytes = max_upload_bytes + 64 * 1024;
constexpr auto start_attribute = "app.requestStart";

void configure_logging(const Config& config, const Logger& log) {
    constexpr trantor::Logger::LogLevel levels[]{
        trantor::Logger::kInfo, trantor::Logger::kWarn, trantor::Logger::kError
    };
    app().setLogLevel(levels[static_cast<int>(config.parsed_log_level())]);
    app().registerPreRoutingAdvice([](const HttpRequestPtr& request) {
        request->attributes()->insert(start_attribute, std::chrono::steady_clock::now());
        api_request_id(*request);
    });
    app().registerPreSendingAdvice([log](const HttpRequestPtr& request,
                                        const HttpResponsePtr& response) {
        using Clock = std::chrono::steady_clock;
        const auto start = request->attributes()->get<Clock::time_point>(start_attribute);
        if (start == Clock::time_point{}) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start);
        auto route = request->matchedPathPattern();
        if (route.empty()) route = request->path().starts_with("/public/") ? "/public/*" : "(unmatched)";
        log.write(LogLevel::info, "{} {} {} {} {}ms", api_request_id(*request), request->methodString(), route,
                  static_cast<int>(response->statusCode()), elapsed.count());
    });
    app().setExceptionHandler([log](const std::exception& error, const HttpRequestPtr& request,
                                   std::function<void(const HttpResponsePtr&)>&& reply) {
        if (dynamic_cast<const WorkersBusy*>(&error)) {
            reply(api_error(*request, k503ServiceUnavailable, "SERVICE_UNAVAILABLE", "Server is busy. Try again shortly."));
            return;
        }
        log.write(LogLevel::error, "Request failed in {}", request->matchedPathPattern());
        reply(api_exception(*request, error));
    });
}

}

int serve(const std::filesystem::path& root) {
    if (sodium_init() < 0) throw std::runtime_error("could not initialize libsodium");
    const auto config = load_config<Config>(root);
    const auto state = root / ".local";
    const auto public_path = root / "public";
    fs::create_directories(state / "empty");
    fs::create_directories(public_path / "uploads");

    auto signing_key = load_signing_key();
    std::shared_ptr<Services> services;
    {
        trantor::EventLoopThread startup_thread;
        startup_thread.run();
        const auto setup = [&]() -> Task<std::shared_ptr<Services>> {
            co_await switchThreadCoro(startup_thread.getLoop());
            auto database = co_await open_sqlite_database(state / "users.sqlite3");
            auto dao = co_await UserDao::init(database);
            auto posts = co_await SocialPostDao::init(database);
            auto comments = co_await SocialCommentDao::init(database);
            auto likes = co_await SocialLikeDao::init(database);
            SocialService social(std::move(posts), std::move(comments), std::move(likes));
            auto initialized = std::make_shared<Services>(root, config, std::move(signing_key),
                                                          UserService(std::move(dao)), std::move(social));
            if (!config.test_user.empty()) {
                co_await initialized->users.configure_demo(config.test_user, config.test_password);
            } else if (const auto credentials = co_await initialized->users.seed_demo()) {
                std::println("Demo account created. Username: {} Password: {}", credentials->username,
                             credentials->password);
                std::fflush(stdout);
            }
            co_return initialized;
        };
        services = sync_wait(setup());
    }

    auto& server = app();
    server.addListener(config.bind_address, static_cast<std::uint16_t>(config.port))
        .setDocumentRoot((state / "empty").string())
        .setUploadPath((state / "request-cache").string())
        .setImplicitPageEnable(false)
        .setClientMaxBodySize(max_request_bytes)
        .setMaxConnectionNum(config.max_connections)
        .setFileTypes({"html", "css", "js", "png", "jpg", "jpeg", "gif", "webp", "pdf", "svg"})
        .addALocation("/public/", "", public_path.string(), true, false, false);

    configure_logging(config, services->log);
    const auto jwt = register_jwt_middleware({services, &services->tokens});
    register_demo_routes();
    register_auth_routes(services, jwt);
    register_social_routes(services, jwt);
    register_upload_routes(services, jwt);
    register_content_routes(services);
    server.setTermSignalHandler([services] { services->stopping.test_and_set(); })
        .setIntSignalHandler([services] { services->stopping.test_and_set(); });
    server.registerBeginningAdvice([services, address = config.bind_address, port = config.port] {
        services->log.write(LogLevel::info, "Listening on http://{}:{}", address, port);
        std::fflush(stdout);
        app().getLoop()->runEvery(0.1, [services] {
            if (services->stopping.test()) {
                services->workers.stop();
                app().quit();
            }
        });
    });
    server.run();
    return 0;
}

}
