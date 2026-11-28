#pragma once

#include "app_namespace.h"
#include "framework/http/server.h"

#include <stdexcept>
#include <string>

namespace APP_NAMESPACE {

struct Config {
    std::string test_user;
    std::string test_password;
    ServerConfig server;

    void validate() const {
        server.validate();
        if (test_user.empty() != test_password.empty() ||
            test_user.size() > 128 || test_password.size() > 1024) {
            throw std::runtime_error(
                "test_user and test_password must be configured together "
                "and limited to 128 and 1024 bytes respectively");
        }
    }
};

}
