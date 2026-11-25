#pragma once

#include "app_namespace.h"

#include "framework/auth/tokens.h"
#include "framework/storage/uploads.h"
#include "app/services/social_service.h"
#include "app/services/user_service.h"
#include "config.h"
#include "framework/support/log.h"
#include "framework/support/workers.h"

#include <atomic>

namespace APP_NAMESPACE {

struct Services {
    Services(const std::filesystem::path& root, const Config& config, std::string key, UserService user_service,
             SocialService social_service)
        : users(std::move(user_service)),
          social(std::move(social_service)),
          tokens(std::move(key), config.jwt_lifetime),
          uploads(root / "public/uploads"),
          log(config.log_level) {}

    UserService users;
    SocialService social;
    Tokens tokens;
    Uploads uploads;
    Logger log;
    Workers workers;
    std::atomic_flag stopping{};
};

}
