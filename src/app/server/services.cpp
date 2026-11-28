#include "services.h"

#include "app/config.h"
#include "app/dao/social_dao.h"
#include "app/dao/user_dao.h"
#include "app/services/password_hasher.h"
#include "framework/http/server_runtime.h"

#include <cstdio>
#include <print>

namespace APP_NAMESPACE {

drogon::Task<std::shared_ptr<Services>>
initialize_services(DatabaseBackendPtr database, const Config& config,
                    const ServerRuntime& runtime) {
    auto users = co_await UserDao::init(database);
    auto posts = co_await SocialPostDao::init(database);
    auto comments = co_await SocialCommentDao::init(database);
    auto likes = co_await SocialLikeDao::init(database);
    auto services = std::make_shared<Services>(
        UserService(std::move(users), PasswordHasher(runtime.workers())),
        SocialService(std::move(posts), std::move(comments), std::move(likes)));
    if (!config.test_user.empty()) {
        co_await services->users.configure_demo(config.test_user,
                                                config.test_password);
    } else if (const auto credentials = co_await services->users.seed_demo()) {
        std::println("Demo account created. Username: {} Password: {}",
                     credentials->username, credentials->password);
        std::fflush(stdout);
    }
    co_return services;
}

}
