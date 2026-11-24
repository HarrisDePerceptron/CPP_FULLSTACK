#include "user_dao.hpp"

namespace APP_NAMESPACE {

drogon::Task<UserDao> UserDao::init(DatabaseBackendPtr database) {
    co_return co_await finishInitialization(UserDao(std::move(database)));
}

UserDao::UserDao(DatabaseBackendPtr database)
    : BaseDao(std::move(database)) {}

drogon::Task<std::optional<UserRecord>> UserDao::findByUsername(std::string username) const {
    auto rows = co_await find(1, 0, selectAll(), where<&UserRecord::username>(std::move(username)));
    if (rows.empty()) co_return std::nullopt;
    co_return std::move(rows.front());
}

drogon::Task<bool> UserDao::hasDemo() const {
    const auto rows = co_await executeSql("SELECT 1 FROM users WHERE seed_key = 'demo'");
    co_return !rows.empty();
}

drogon::Task<bool> UserDao::insertDemo(std::string username, std::string password_hash) const {
    const auto result = co_await executeSql(
        "INSERT INTO users (username, password_hash, seed_key) "
        "VALUES (" + parameter(1) + ", " + parameter(2) + ", 'demo') ON CONFLICT(seed_key) DO NOTHING",
        std::move(username), std::move(password_hash));
    co_return result.affectedRows() == 1;
}

drogon::Task<> UserDao::replaceDemo(std::string username, std::string password_hash) const {
    co_await executeSql("INSERT INTO users (username, password_hash, seed_key) "
                        "VALUES (" + parameter(1) + ", " + parameter(2) + ", 'demo') ON CONFLICT(seed_key) DO UPDATE SET "
                        "username = excluded.username, password_hash = excluded.password_hash",
                        std::move(username), std::move(password_hash));
}

}
