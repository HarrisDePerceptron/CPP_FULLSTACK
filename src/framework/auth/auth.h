#pragma once

#include "tokens.h"

#include <drogon/HttpMiddleware.h>
#include <drogon/utils/HttpConstraint.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

inline constexpr auto session_cookie_name = "app_session";

const std::string& get_current_username(const drogon::HttpRequest& request);

namespace detail {

class AuthVerifierScope {
  public:
    explicit AuthVerifierScope(std::shared_ptr<const Tokens> verifier);
    ~AuthVerifierScope();
    AuthVerifierScope(const AuthVerifierScope&) = delete;
    AuthVerifierScope& operator=(const AuthVerifierScope&) = delete;

  private:
    std::shared_ptr<const Tokens> previous_;
};

drogon::Task<drogon::HttpResponsePtr>
verify_request(drogon::HttpRequestPtr request,
               drogon::MiddlewareNextAwaiter next,
               std::span<const std::string_view> roles);

template <auto... roles>
class AuthPolicyMiddleware final
    : public drogon::HttpCoroMiddleware<AuthPolicyMiddleware<roles...>> {
  public:
    drogon::Task<drogon::HttpResponsePtr>
    invoke(const drogon::HttpRequestPtr& request,
           drogon::MiddlewareNextAwaiter&& next) override {
        return verify_request(request, std::move(next), allowed_roles);
    }

  private:
    static constexpr std::array<std::string_view, sizeof...(roles)>
        allowed_roles{magic_enum::enum_name(roles)...};
};

}

template <auto... roles>
    requires(RoleEnum<decltype(roles)> && ...)
struct Auth {
    static_assert((!magic_enum::enum_name(roles).empty() && ...),
                  "Unknown role enum value");

    operator drogon::internal::HttpConstraint() const {
        return {detail::AuthPolicyMiddleware<roles...>::classTypeName()};
    }
};

}
