#pragma once

#include "app_namespace.h"

#include <memory>
#include <string>

namespace drogon {
class HttpAppFramework;
}

namespace APP_NAMESPACE {

inline constexpr auto subject_attribute = "app.username";
inline constexpr auto session_cookie_name = "app_session";
class Tokens;

std::string register_jwt_middleware(drogon::HttpAppFramework& server,
                                    std::shared_ptr<const Tokens> tokens);

}
