#include "app_namespace.hpp"
#include "server.hpp"

#include "auth/jwt_middleware.hpp"
#include "json_response.hpp"
#include "routes/routes.hpp"
#include "services.hpp"

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

constexpr std::size_t maxRequestBytes = maxUploadBytes + 64 * 1024;
constexpr auto startAttribute = "app.requestStart";

void configureLogging(const Config& config, const Logger& log) {
    constexpr trantor::Logger::LogLevel levels[]{
        trantor::Logger::kInfo, trantor::Logger::kWarn, trantor::Logger::kError
    };
    app().setLogLevel(levels[static_cast<int>(config.logLevel)]);
    app().registerPreRoutingAdvice([](const HttpRequestPtr& request) {
        request->attributes()->insert(startAttribute, std::chrono::steady_clock::now());
    });
    app().registerPreSendingAdvice([log](const HttpRequestPtr& request,
                                        const HttpResponsePtr& response) {
        using Clock = std::chrono::steady_clock;
        const auto start = request->attributes()->get<Clock::time_point>(startAttribute);
        if (start == Clock::time_point{}) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start);
        auto route = request->matchedPathPattern();
        if (route.empty()) route = request->path().starts_with("/public/") ? "/public/*" : "(unmatched)";
        log.write(LogLevel::info, "{} {} {} {}ms", request->methodString(), route,
                  static_cast<int>(response->statusCode()), elapsed.count());
    });
    app().setExceptionHandler([log](const std::exception& error, const HttpRequestPtr& request,
                                   std::function<void(const HttpResponsePtr&)>&& reply) {
        if (dynamic_cast<const WorkersBusy*>(&error)) {
            reply(jsonError(k503ServiceUnavailable, "Server is busy. Try again shortly."));
            return;
        }
        log.write(LogLevel::error, "Request failed in {}", request->matchedPathPattern());
        reply(jsonError(k500InternalServerError, "Internal server error."));
    });
}

}

int serve(const std::filesystem::path& root) {
    if (sodium_init() < 0) throw std::runtime_error("could not initialize libsodium");
    loadEnvironment(root / ".env");
    const auto config = readConfig(root / "config.json");
    const auto state = root / ".local";
    const auto publicPath = root / "public";
    fs::create_directories(state / "empty");
    fs::create_directories(publicPath / "uploads");

    auto signingKey = loadSigningKey();
    std::shared_ptr<Services> services;
    {
        trantor::EventLoopThread startupThread;
        startupThread.run();
        const auto setup = [&]() -> Task<std::shared_ptr<Services>> {
            co_await switchThreadCoro(startupThread.getLoop());
            auto database = co_await openSqliteDatabase(state / "users.sqlite3");
            auto dao = co_await UserDao::init(std::move(database));
            auto initialized = std::make_shared<Services>(root, config, std::move(signingKey),
                                                          UserService(std::move(dao)));
            if (!config.testUser.empty()) {
                co_await initialized->users.configureDemo(config.testUser, config.testPassword);
            } else if (const auto credentials = co_await initialized->users.seedDemo()) {
                std::println("Demo account created. Username: {} Password: {}", credentials->username,
                             credentials->password);
                std::fflush(stdout);
            }
            co_return initialized;
        };
        services = sync_wait(setup());
    }

    auto& server = app();
    server.addListener(config.bindAddress, static_cast<std::uint16_t>(config.port))
        .setDocumentRoot((state / "empty").string())
        .setUploadPath((state / "request-cache").string())
        .setImplicitPageEnable(false)
        .setClientMaxBodySize(maxRequestBytes)
        .setMaxConnectionNum(config.maxConnections)
        .setFileTypes({"html", "css", "js", "png", "jpg", "jpeg", "gif", "webp", "pdf", "svg"})
        .addALocation("/public/", "", publicPath.string(), true, false, false);

    configureLogging(config, services->log);
    const auto jwt = registerJwtMiddleware({services, &services->tokens});
    registerDemoRoutes();
    registerAuthRoutes(services, jwt);
    registerUploadRoutes(services, jwt);
    registerContentRoutes(services);
    server.setTermSignalHandler([services] { services->stopping.test_and_set(); })
        .setIntSignalHandler([services] { services->stopping.test_and_set(); });
    server.registerBeginningAdvice([services, address = config.bindAddress, port = config.port] {
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
