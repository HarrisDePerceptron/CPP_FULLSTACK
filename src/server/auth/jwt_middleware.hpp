#pragma once

#include "app_namespace.hpp"

#include <memory>
#include <string>

namespace APP_NAMESPACE {

inline constexpr auto subjectAttribute = "app.username";
inline constexpr auto sessionCookieName = "app_session";
class Tokens;

std::string registerJwtMiddleware(std::shared_ptr<const Tokens> tokens);

}
