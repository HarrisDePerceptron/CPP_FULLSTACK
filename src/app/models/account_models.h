#pragma once

#include "app_namespace.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

struct Credentials {
    std::string username;
    std::string password;
};

struct LoginResult {
    std::string access_token;
    std::string_view token_type;
    std::int64_t expires_in;
};

struct Identity {
    std::string username;
};

struct UserMatch {
    std::int64_t id;
    std::string username;
};

}
