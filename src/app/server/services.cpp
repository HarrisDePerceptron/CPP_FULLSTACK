#include "services.h"

#include "app/dao/social_dao.h"
#include "app/dao/user_dao.h"
#include "framework/db/database_backend.h"

#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoopThread.h>

#include <cstdio>
#include <print>

namespace APP_NAMESPACE {

std::shared_ptr<Services> initialize_services(const std::filesystem::path& root, const Config& config) {
    auto signing_key = load_signing_key();
    trantor::EventLoopThread startup_thread;
    startup_thread.run();

    const auto setup = [&]() -> drogon::Task<std::shared_ptr<Services>> {
        co_await drogon::switchThreadCoro(startup_thread.getLoop());
        auto database = co_await open_sqlite_database(root / ".local" / "users.sqlite3");
        auto users = co_await UserDao::init(database);
        auto posts = co_await SocialPostDao::init(database);
        auto comments = co_await SocialCommentDao::init(database);
        auto likes = co_await SocialLikeDao::init(database);
        SocialService social(std::move(posts), std::move(comments), std::move(likes));
        auto services = std::make_shared<Services>(root, config, std::move(signing_key),
                                                   UserService(std::move(users)), std::move(social));
        if (!config.test_user.empty()) {
            co_await services->users.configure_demo(config.test_user, config.test_password);
        } else if (const auto credentials = co_await services->users.seed_demo()) {
            std::println("Demo account created. Username: {} Password: {}", credentials->username,
                         credentials->password);
            std::fflush(stdout);
        }
        co_return services;
    };
    return drogon::sync_wait(setup());
}

}
