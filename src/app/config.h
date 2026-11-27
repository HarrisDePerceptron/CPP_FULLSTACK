#pragma once

#include "app_namespace.h"
#include "framework/http/server.h"
#include "framework/support/validation.h"

#include <stdexcept>
#include <string>

namespace APP_NAMESPACE {

struct Config {
    int jwt_lifetime_seconds{};
    std::string test_user;
    std::string test_password;
    ServerConfig server;

    void validate() const {
        server.validate();
        require_positive_number(jwt_lifetime_seconds, 86400,
                                "jwt_lifetime_seconds");

        if (test_user.empty() != test_password.empty() ||
            test_user.size() > 128 || test_password.size() > 1024) {
            throw std::runtime_error(
                "test_user and test_password must be configured together "
                "and limited to 128 and 1024 bytes respectively");
        }
    }
};

}
