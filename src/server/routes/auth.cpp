#include "app_namespace.hpp"
#include "routes.hpp"
#include "server/json_response.hpp"
#include "server/services.hpp"
#include "server/auth/jwt_middleware.hpp"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {
struct LoginResult {
    std::string access_token;
    std::string_view token_type;
    std::int64_t expires_in;
};

struct Identity {
    std::string username;
};

namespace {

using namespace drogon;

Cookie sessionCookie(const HttpRequestPtr& request, std::string value, int maxAge) {
    Cookie cookie(sessionCookieName, std::move(value));
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSameSite(Cookie::SameSite::kStrict);
    cookie.setSecure(request->isOnSecureConnection());
    cookie.setMaxAge(maxAge);
    return cookie;
}

Credentials readCredentials(const HttpRequestPtr& request, std::size_t maxBodyBytes) {
    Credentials credentials;
    const auto body = request->body();
    if (request->contentType() != CT_APPLICATION_JSON || body.size() > maxBodyBytes ||
        glz::validate_json(body) ||
        glz::read<glz::opts{.error_on_missing_keys = true}>(credentials, body) ||
        credentials.username.empty() || credentials.username.size() > 128 ||
        credentials.password.empty() || credentials.password.size() > 1024) {
        throw RequestValidationError("Send a JSON username and password.");
    }
    return credentials;
}

Task<HttpResponsePtr> signup(std::shared_ptr<Services> services, HttpRequestPtr request) {
    auto credentials = readCredentials(request, 8192);
    const auto& name = credentials.username;
    if (name.size() < 3 || name.size() > 32 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        credentials.password.size() < 8) {
        throw RequestValidationError(
            "Use 3–32 letters, digits, underscores, or hyphens for the username, and at least 8 bytes for the password.");
    }
    Identity identity{name};
    if (!co_await services->users.signup(std::move(credentials), services->workers)) {
        co_return apiError(*request, k409Conflict, "CONFLICT", "That username is already taken.");
    }
    co_return apiOk(*request, std::move(identity), k201Created);
}

Task<HttpResponsePtr> login(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto credentials = readCredentials(request, 4096);
    if (!co_await services->users.authenticate(credentials.username, credentials.password, services->workers)) {
        co_return apiError(*request, k401Unauthorized, "UNAUTHORIZED", "Invalid username or password.");
    }

    const auto token = services->tokens.issue(credentials.username);
    const auto lifetime = services->tokens.lifetime().count();
    auto response = apiOk(*request, LoginResult{token, "Bearer", lifetime});
    response->addCookie(sessionCookie(request, token, static_cast<int>(lifetime)));
    response->addHeader("Cache-Control", "no-store");
    co_return response;
}

}

void registerAuthRoutes(const std::shared_ptr<Services>& services, const std::string& jwt) {
    auto& server = app();
    server.registerHandler("/api/signup", [services](HttpRequestPtr request) {
        return signup(services, std::move(request));
    }, {Post});
    server.registerHandler("/api/login", [services](HttpRequestPtr request) {
        return login(services, std::move(request));
    }, {Post});
    server.registerHandler("/api/logout", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        auto response = apiOk(*request);
        response->addCookie(sessionCookie(request, "expired", 0));
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }, {Post});
    server.registerHandler("/api/me", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        auto response = apiOk(*request, Identity{request->attributes()->get<std::string>(
            subjectAttribute)});
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }, {Get, jwt});
}

}
