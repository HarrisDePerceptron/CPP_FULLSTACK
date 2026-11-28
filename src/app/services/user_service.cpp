#include "app_namespace.h"
#include "user_service.h"
#include "framework/support/random.h"

namespace APP_NAMESPACE {

drogon::Task<std::optional<Credentials>> UserService::seed_demo() const {
    if (co_await dao_.has_demo())
        co_return std::nullopt;

    Credentials credentials{"demo-" + random_hex<6>(), random_hex<16>()};
    auto hash = co_await passwords_.hash(credentials.password);
    if (!co_await dao_.insert_demo(credentials.username, std::move(hash)))
        co_return std::nullopt;
    co_return credentials;
}

drogon::Task<> UserService::configure_demo(std::string username,
                                           std::string password) const {
    auto hash = co_await passwords_.hash(std::move(password));
    co_await dao_.replace_demo(std::move(username), std::move(hash));
}

drogon::Task<std::optional<UserRecord::Id>>
UserService::find_id(std::string username) const {
    const auto user = co_await dao_.find_by_username(std::move(username));
    co_return user ? user->id : std::nullopt;
}

drogon::Task<bool> UserService::signup(Credentials credentials) const {
    auto hash = co_await passwords_.hash(std::move(credentials.password));
    co_return co_await dao_.insert_account(std::move(credentials.username),
                                           std::move(hash));
}

drogon::Task<std::optional<std::vector<Role>>>
UserService::authenticate(std::string username, std::string password) const {
    const auto user = co_await dao_.find_by_username(std::move(username));
    if (!user)
        co_return std::nullopt;
    const auto verified =
        co_await passwords_.verify(std::move(password), user->password_hash);
    if (!verified)
        co_return std::nullopt;
    co_return UserDao::read_roles(*user);
}

}
