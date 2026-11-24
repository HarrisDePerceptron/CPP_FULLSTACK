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

    const auto token = services->tokens.issue(credentials.username);
    const auto lifetime = services->tokens.lifetime().count();
    auto response = jsonResponse(LoginResult{token, "Bearer", lifetime});
    response->addCookie(sessionCookie(request, token, static_cast<int>(lifetime)));
    response->addHeader("Cache-Control", "no-store");
    co_return response;
}

}

void registerAuthRoutes(const std::shared_ptr<Services>& services, const std::string& jwt) {
    auto& server = app();
    server.registerHandler("/api/login", [services](HttpRequestPtr request) {
        return login(services, std::move(request));
    }, {Post});
    server.registerHandler("/api/logout", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        auto response = HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
        response->addCookie(sessionCookie(request, "expired", 0));
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }, {Post});
    server.registerHandler("/api/me", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        auto response = jsonResponse(Identity{request->attributes()->get<std::string>(
            subjectAttribute)});
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }, {Get, jwt});
}

}
