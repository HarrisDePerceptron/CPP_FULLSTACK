#include "auth.h"
#include "framework/http/json_response.h"

#include <algorithm>
#include <mutex>
#include <strings.h>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;
constexpr auto username_attribute = "app.username";
std::mutex verifier_mutex;
std::shared_ptr<const Tokens> token_verifier;

}

const std::string& get_current_username(const drogon::HttpRequest& request) {
    const auto& username =
        request.attributes()->get<std::string>(username_attribute);
    if (username.empty())
        throw std::logic_error("Current username requires Auth middleware");
    return username;
}

namespace detail {

AuthVerifierScope::AuthVerifierScope(std::shared_ptr<const Tokens> verifier) {
    const std::lock_guard lock(verifier_mutex);
    previous_ = std::exchange(token_verifier, std::move(verifier));
}

AuthVerifierScope::~AuthVerifierScope() {
    const std::lock_guard lock(verifier_mutex);
    token_verifier.swap(previous_);
}

Task<HttpResponsePtr> verify_request(HttpRequestPtr request,
                                     MiddlewareNextAwaiter next,
                                     std::span<const std::string_view> roles) {
    const auto verifier = [] {
        const std::lock_guard lock(verifier_mutex);
        return token_verifier;
    }();
    if (!verifier)
        throw std::logic_error(
            "Auth middleware requires a configured token verifier");

    const auto& header = request->getHeader("authorization");
    constexpr std::string_view prefix = "Bearer ";
    std::string_view token;
    if (header.empty()) {
        token = request->getCookie(session_cookie_name);
    } else if (header.size() > prefix.size() &&
               strncasecmp(header.data(), prefix.data(), prefix.size()) == 0) {
        token = std::string_view(header).substr(prefix.size());
    }

    auto verified = verifier->verify(token);
    if (!verified) {
        auto response =
            api_error(*request, k401Unauthorized, "UNAUTHORIZED",
                      "A valid session or Bearer token is required.");
        response->addHeader("WWW-Authenticate", "Bearer");
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }
    if (!roles.empty() &&
        !std::ranges::any_of(roles, [&](std::string_view role) {
            return std::ranges::binary_search(verified->role_names, role);
        })) {
        auto response = api_error(*request, k403Forbidden, "FORBIDDEN",
                                  "This route requires an assigned role.");
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }

    request->attributes()->insert(username_attribute,
                                  std::move(verified->subject));
    co_return co_await std::move(next);
}

}
}
