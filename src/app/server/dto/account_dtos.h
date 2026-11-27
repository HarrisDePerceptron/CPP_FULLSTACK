#pragma once

#include "app_namespace.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

struct LoginResult {
    std::string access_token;
    std::string_view token_type;
    std::int64_t expires_in;
};

struct Identity {
    std::string username;
};

}
