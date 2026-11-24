#pragma once

#include "app_namespace.hpp"

#include "server/auth/tokens.hpp"
#include "services/uploads.hpp"
#include "services/user_service.hpp"
#include "utils/config.hpp"
#include "utils/log.hpp"
#include "utils/workers.hpp"

#include <atomic>

namespace APP_NAMESPACE {

struct Services {
    Services(const std::filesystem::path& root, const Config& config, std::string key)
        : users(root / ".local/users.sqlite3"),
          tokens(std::move(key), config.jwtLifetime),
          uploads(root / "public/uploads"),
          log(config.logLevel) {}

    UserService users;
    Tokens tokens;
    Uploads uploads;
    Logger log;
    Workers workers;
    std::atomic_flag stopping{};
};

}
