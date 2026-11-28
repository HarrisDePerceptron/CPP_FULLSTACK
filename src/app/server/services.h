#pragma once

#include "app_namespace.h"

#include "app/services/social_service.h"
#include "app/services/user_service.h"
#include "framework/db/database_backend.h"

#include <memory>

namespace APP_NAMESPACE {

struct Config;

struct Services {
    UserService users;
    SocialService social;
};

drogon::Task<std::shared_ptr<Services>>
initialize_services(DatabaseBackendPtr database, const Config& config);

}
