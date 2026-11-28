#pragma once

#include "tokens.h"

#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace drogon {
class HttpAppFramework;
class HttpRequest;
}

namespace APP_NAMESPACE {

inline constexpr auto session_cookie_name = "app_session";

const std::string& get_current_username(const drogon::HttpRequest& request);

class Auth {
  public:
    Auth(drogon::HttpAppFramework& server,
         std::shared_ptr<const Tokens> tokens);
    Auth(const Auth&) = delete;
    Auth& operator=(const Auth&) = delete;

    std::string operator()() { return this->register_policy({}); }

    template <RoleEnum E> std::string operator()(E role) {
        return this->operator()(std::initializer_list<E>{role});
    }

    template <RoleEnum E>
    std::string operator()(std::initializer_list<E> roles) {
        if (roles.size() == 0)
            throw std::invalid_argument(
                "A role policy needs at least one role");
        std::vector<std::string> names;
        names.reserve(roles.size());
        for (const auto role : roles)
            names.emplace_back(role_name(role));
        return this->register_policy(std::move(names));
    }

  private:
    std::string register_policy(std::vector<std::string> roles);

    drogon::HttpAppFramework& server_;
    std::shared_ptr<const Tokens> tokens_;
    std::string policy_prefix_;
    std::set<std::string> registered_policies_;
};

}
