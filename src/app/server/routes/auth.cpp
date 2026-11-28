#include "app/server/dto/account_dtos.h"
#include "app/server/routes.h"
#include "app/server/services.h"
#include "framework/auth/auth.h"
#include "framework/http/json_response.h"
#include "framework/http/server_runtime.h"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {

namespace {

using namespace drogon;

Cookie session_cookie(const HttpRequestPtr& request, std::string value,
                      int max_age) {
    Cookie cookie(session_cookie_name, std::move(value));
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSameSite(Cookie::SameSite::kStrict);
    cookie.setSecure(request->isOnSecureConnection());
    cookie.setMaxAge(max_age);
    return cookie;
}

Credentials read_credentials(const HttpRequestPtr& request,
                             std::size_t max_body_bytes) {
    Credentials credentials;
    const auto body = request->body();
    if (request->contentType() != CT_APPLICATION_JSON ||
        body.size() > max_body_bytes || glz::validate_json(body) ||
        glz::read<glz::opts{.error_on_missing_keys = true}>(credentials,
                                                            body) ||
        credentials.username.empty() || credentials.username.size() > 128 ||
        credentials.password.empty() || credentials.password.size() > 1024) {
        throw RequestValidationError("Send a JSON username and password.");
    }
    return credentials;
}

Task<HttpResponsePtr> signup(std::shared_ptr<Services> services,
                             HttpRequestPtr request) {
    auto credentials = read_credentials(request, 8192);
    const auto& name = credentials.username;
    if (name.size() < 3 || name.size() > 32 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                               "VWXYZ0123456789_-") != std::string::npos ||
        credentials.password.size() < 8) {
        throw RequestValidationError(
            "Use 3–32 letters, digits, underscores, or hyphens for the "
            "username, and at least 8 bytes for the password.");
    }
    Identity identity{name};
    if (!co_await services->users.signup(std::move(credentials))) {
        co_return api_error(*request, k409Conflict, "CONFLICT",
                            "That username is already taken.");
    }
    co_return api_ok(*request, std::move(identity), k201Created);
}

Task<HttpResponsePtr> login(std::shared_ptr<Services> services,
                            const Tokens& tokens, HttpRequestPtr request) {
    const auto credentials = read_credentials(request, 4096);
    const auto roles = co_await services->users.authenticate(
        credentials.username, credentials.password);
    if (!roles) {
        co_return api_error(*request, k401Unauthorized, "UNAUTHORIZED",
                            "Invalid username or password.");
    }

    std::vector<std::string> names;
    names.reserve(roles->size());
    for (const auto role : *roles)
        names.emplace_back(role_name(role));
    const auto token = tokens.issue(credentials.username, names);
    const auto lifetime = tokens.lifetime().count();
    auto response = api_ok(*request, LoginResult{token, "Bearer", lifetime});
    response->addCookie(
        session_cookie(request, token, static_cast<int>(lifetime)));
    response->addHeader("Cache-Control", "no-store");
    co_return response;
}

Task<HttpResponsePtr> logout(HttpRequestPtr request) {
    auto response = api_ok(*request);
    response->addCookie(session_cookie(request, "expired", 0));
    response->addHeader("Cache-Control", "no-store");
    co_return response;
}

Task<HttpResponsePtr> current_user(HttpRequestPtr request) {
    auto response = api_ok(*request, Identity{get_current_username(*request)});
    response->addHeader("Cache-Control", "no-store");
    co_return response;
}

}

void register_auth_routes(ServerRuntime& runtime,
                          const std::shared_ptr<Services>& services) {
    auto routes = runtime.routes<"/api">();
    auto accounts = routes.with(services)
                        .post<"/signup">(signup)
                        .with(std::cref(runtime.tokens()))
                        .post<"/login">(login);
    routes.post<"/logout">(logout)
        .get<"/me">(Auth(), current_user)
        .merge(accounts)
        .build();
}

}
