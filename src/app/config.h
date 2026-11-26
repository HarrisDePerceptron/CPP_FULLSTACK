#pragma once

#include "app_namespace.h"
#include "framework/support/log.h"
#include "framework/support/validation.h"

#include <arpa/inet.h>

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace APP_NAMESPACE {

struct Config {
    std::string bind_address = "127.0.0.1";
    int port{};
    std::size_t max_connections = 64;
    int jwt_lifetime_seconds{};
    std::string log_level;
    std::string test_user;
    std::string test_password;

    void validate() const {
        in_addr parsed{};
        if (bind_address.find('\0') != std::string::npos ||
            inet_pton(AF_INET, bind_address.c_str(), &parsed) != 1) {
            throw std::runtime_error("bind_address must be an IPv4 address");
        }
        require_positive_number(port, 65535, "port");
        require_positive_number(max_connections, std::numeric_limits<std::size_t>::max(), "max_connections");
        require_positive_number(jwt_lifetime_seconds, 86400, "jwt_lifetime_seconds");
        parse_log_level(log_level);

        if (test_user.empty() != test_password.empty() ||
            test_user.size() > 128 || test_password.size() > 1024) {
            throw std::runtime_error("test_user and test_password must be configured together "
                                     "and limited to 128 and 1024 bytes respectively");
        }
    }
};

}
