#include "server.hpp"

#include "config.hpp"
#include "content.hpp"
#include "json_response.hpp"
#include "log.hpp"
#include "tokens.hpp"
#include "uploads.hpp"
#include "users.hpp"
#include "workers.hpp"

#include <drogon/HttpMiddleware.h>
#include <drogon/drogon.h>
#include <sodium.h>
#include <strings.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace hello {

struct LoginResult {
    std::string access_token;
    std::string_view token_type;
    std::int64_t expires_in;
};

struct Identity {
    std::string username;
};

struct UploadResult {
    std::string url;
};

namespace {

using namespace drogon;
namespace fs = std::filesystem;

constexpr std::size_t maxRequestBytes = maxUploadBytes + 64 * 1024;
constexpr auto subjectAttribute = "hello.username";
constexpr auto startAttribute = "hello.requestStart";
constexpr auto uploadPrefix = "/public/uploads/";

struct Services {
    Services(const fs::path& root, const Config& config, std::string key)
        : users(root / ".local/users.sqlite3"),
          tokens(std::move(key), config.jwtLifetime),
          uploads(root / "public/uploads"),
          log(config.logLevel) {}

    Users users;
    Tokens tokens;
    Uploads uploads;
    Logger log;
    Workers workers;
    std::atomic_flag stopping{};
};

class JwtMiddleware : public HttpCoroMiddleware<JwtMiddleware, false> {
public:
    explicit JwtMiddleware(std::shared_ptr<const Services> services)
        : services_(std::move(services)) {}

    Task<HttpResponsePtr> invoke(const HttpRequestPtr& request,
                                 MiddlewareNextAwaiter&& next) override {
        const auto& header = request->getHeader("authorization");
        constexpr std::string_view prefix = "Bearer ";
        if (header.size() > prefix.size() &&
            strncasecmp(header.data(), prefix.data(), prefix.size()) == 0) {
            const auto token = std::string_view(header).substr(prefix.size());
            if (auto username = services_->tokens.verify(token)) {
                request->attributes()->insert(subjectAttribute, std::move(*username));
                co_return co_await std::move(next);
            }
        }

        auto response = jsonError(k401Unauthorized, "A valid Bearer token is required.");
        response->addHeader("WWW-Authenticate", "Bearer");
        co_return response;
    }

private:
    std::shared_ptr<const Services> services_;
};

Task<HttpResponsePtr> login(std::shared_ptr<Services> services, HttpRequestPtr request) {
    Credentials credentials;
    const auto body = request->body();
    if (request->contentType() != CT_APPLICATION_JSON || body.size() > 4096) {
        co_return jsonError(k400BadRequest, "Send a JSON username and password.");
    }
    const std::string json(body);
    if (glz::validate_json(json) ||
        glz::read<glz::opts{.error_on_missing_keys = true}>(credentials, json) ||
        credentials.username.empty() || credentials.username.size() > 128 ||
        credentials.password.empty() || credentials.password.size() > 1024) {
        co_return jsonError(k400BadRequest, "Send a JSON username and password.");
    }

    if (!co_await services->workers.submit([&] {
        return services->users.authenticate(credentials.username, credentials.password);
    })) {
        co_return jsonError(k401Unauthorized, "Invalid username or password.");
    }

    auto response = jsonResponse(LoginResult{services->tokens.issue(credentials.username), "Bearer",
                                             services->tokens.lifetime().count()});
    response->addHeader("Cache-Control", "no-store");
    co_return response;
}

Task<HttpResponsePtr> upload(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto saved = co_await services->workers.submit([&]
        -> std::expected<std::string, UploadError> {
        MultiPartParser parser;
        if (parser.parse(request) != 0 || parser.getFiles().size() != 1 ||
            parser.getFiles().front().getItemName() != "file") {
            return std::unexpected(UploadError::empty);
        }
        return services->uploads.save(parser.getFiles().front().fileContent());
    });

    if (!saved) {
        switch (saved.error()) {
            case UploadError::empty:
                co_return jsonError(k400BadRequest, "Choose one file to upload.");
            case UploadError::tooLarge:
                co_return jsonError(k413RequestEntityTooLarge, "The file exceeds 10 MiB.");
            case UploadError::unsupported:
                co_return jsonError(k415UnsupportedMediaType, "Choose a PDF or a supported image.");
        }
    }

    const auto url = std::string(uploadPrefix) + *saved;
    auto response = jsonResponse(UploadResult{url}, k201Created);
    response->addHeader("Location", url);
    co_return response;
}

Task<HttpResponsePtr> download(std::shared_ptr<Services> services, HttpRequestPtr request,
                               std::string name) {
    co_return co_await services->workers.submit([&] {
        const auto file = services->uploads.find(name);
        if (!file) return jsonError(k404NotFound, "Upload not found.");
        auto response = HttpResponse::newFileResponse(file->first.string(), name, CT_CUSTOM,
                                                      std::string(file->second.mime), request);
        response->addHeader("X-Content-Type-Options", "nosniff");
        return response;
    });
}

Task<HttpResponsePtr> content(std::shared_ptr<Services> services) {
    const auto result = co_await services->workers.submit([] { return fetchContent(); });
    if (result) co_return jsonResponse(*result);

    if (result.error() == ContentError::timeout) {
        services->log.write(LogLevel::warn, "Content provider timed out");
        co_return jsonError(k504GatewayTimeout, "The content provider timed out.");
    }
    services->log.write(LogLevel::warn, "Content provider returned an unusable response");
    co_return jsonError(k502BadGateway, "Could not fetch content from the provider.");
}

Task<HttpResponsePtr> home(std::shared_ptr<Services> services, std::string index) {
    co_return co_await services->workers.submit([&] {
        return HttpResponse::newFileResponse(index);
    });
}

void registerRoutes(const fs::path& publicPath, const std::shared_ptr<Services>& services) {
    auto& server = app();
    const auto jwt = JwtMiddleware::classTypeName();
    server.registerMiddleware(std::make_shared<JwtMiddleware>(services));
    server.registerHandler(
        "/", [services, index = (publicPath / "index.html").string()](HttpRequestPtr) {
            return home(services, index);
        }, {Get});
    server.registerHandler("/hello", [](HttpRequestPtr) -> Task<HttpResponsePtr> {
        auto response = HttpResponse::newHttpResponse();
        response->setContentTypeCode(CT_TEXT_PLAIN);
        response->setBody("hello world!");
        co_return response;
    }, {Get});
    server.registerHandler("/api/login", [services](HttpRequestPtr request) {
        return login(services, std::move(request));
    }, {Post});
    server.registerHandler("/api/me", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        auto response = jsonResponse(Identity{request->attributes()->get<std::string>(
            subjectAttribute)});
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }, {Get, jwt});
    server.registerHandler("/api/uploads", [services](HttpRequestPtr request) {
        return upload(services, std::move(request));
    }, {Post, jwt});
    server.registerHandler("/public/uploads/{1}", [services](HttpRequestPtr request,
                                                              std::string name) {
        return download(services, std::move(request), std::move(name));
    }, {Get});
    server.registerHandler("/api/content", [services](HttpRequestPtr) {
        return content(services);
    }, {Get});
}

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
    const auto config = readConfig(root / "config.json");
    const auto state = root / ".local";
    const auto publicPath = root / "public";
    fs::create_directories(state / "empty");
    fs::create_directories(publicPath / "uploads");

    auto services = std::make_shared<Services>(root, config, loadSigningKey(root / ".env"));
    if (const auto credentials = services->users.seedDemo()) {
        std::println("Demo account created. Username: {} Password: {}", credentials->username,
                     credentials->password);
        std::fflush(stdout);
    }

    auto& server = app();
    server.addListener("127.0.0.1", static_cast<std::uint16_t>(config.port))
        .setDocumentRoot((state / "empty").string())
        .setUploadPath((state / "request-cache").string())
        .setImplicitPageEnable(false)
        .setClientMaxBodySize(maxRequestBytes)
        .setMaxConnectionNum(64)
        .setFileTypes({"html", "css", "js", "png", "jpg", "jpeg", "gif", "webp", "pdf", "svg"})
        .addALocation("/public/", "", publicPath.string(), true, false, false);

    configureLogging(config, services->log);
    registerRoutes(publicPath, services);
    server.setTermSignalHandler([services] { services->stopping.test_and_set(); })
        .setIntSignalHandler([services] { services->stopping.test_and_set(); });
    server.registerBeginningAdvice([services, port = config.port] {
        services->log.write(LogLevel::info, "Listening on http://127.0.0.1:{}", port);
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
