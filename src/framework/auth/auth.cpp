#include "auth.h"
#include "framework/http/json_response.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpMiddleware.h>

#include <algorithm>
#include <atomic>
#include <strings.h>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;
constexpr auto username_attribute = "app.username";
std::atomic_size_t next_auth_id{0};

class AuthMiddleware : public HttpCoroMiddleware<AuthMiddleware, false> {
  public:
    AuthMiddleware(std::string name, std::shared_ptr<const Tokens> tokens,
                   std::vector<std::string> roles)
        : name_(std::move(name)), tokens_(std::move(tokens)),
          roles_(std::move(roles)) {}

    const std::string& className() const override { return name_; }

    Task<HttpResponsePtr> invoke(const HttpRequestPtr& request,
                                 MiddlewareNextAwaiter&& next) override {
        const auto& header = request->getHeader("authorization");
        constexpr std::string_view prefix = "Bearer ";
        std::string_view token;
        if (header.empty()) {
            token = request->getCookie(session_cookie_name);
        } else if (header.size() > prefix.size() &&
                   strncasecmp(header.data(), prefix.data(), prefix.size()) ==
                       0) {
            token = std::string_view(header).substr(prefix.size());
        }

        auto verified = tokens_->verify(token);
        if (!verified) {
            auto response =
                api_error(*request, k401Unauthorized, "UNAUTHORIZED",
                          "A valid session or Bearer token is required.");
            response->addHeader("WWW-Authenticate", "Bearer");
            response->addHeader("Cache-Control", "no-store");
            co_return response;
        }
        if (!roles_.empty() &&
            !std::ranges::any_of(roles_, [&](const auto& role) {
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

  private:
    std::string name_;
    std::shared_ptr<const Tokens> tokens_;
    std::vector<std::string> roles_;
};

}

const std::string& get_current_username(const drogon::HttpRequest& request) {
    const auto& username =
        request.attributes()->get<std::string>(username_attribute);
    if (username.empty())
        throw std::logic_error("Current username requires Auth middleware");
    return username;
}

Auth::Auth(drogon::HttpAppFramework& server,
           std::shared_ptr<const Tokens> tokens)
    : server_(server), tokens_(std::move(tokens)),
      policy_prefix_(AuthMiddleware::classTypeName() + ':' +
                     std::to_string(next_auth_id.fetch_add(
                         1, std::memory_order_relaxed))) {
    if (!tokens_)
        throw std::invalid_argument("Auth requires a token verifier");
}

std::string Auth::register_policy(std::vector<std::string> roles) {
    std::ranges::sort(roles);
    const auto duplicates = std::ranges::unique(roles);
    roles.erase(duplicates.begin(), duplicates.end());
    auto name = policy_prefix_;
    for (const auto& role : roles) {
        name += ':';
        name += role;
    }
    if (!registered_policies_.contains(name)) {
        server_.registerMiddleware(
            std::make_shared<AuthMiddleware>(name, tokens_, std::move(roles)));
        registered_policies_.insert(name);
    }
    return name;
}

}
