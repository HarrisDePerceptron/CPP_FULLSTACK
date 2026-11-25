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

drogon::Task<std::vector<UserMatch>> UserDao::search(std::string query) const {
    const auto rows = co_await executeSql(
        "SELECT id, username FROM users WHERE deleted_at IS NULL AND instr(lower(username), lower(?1)) > 0 "
        "ORDER BY CASE WHEN lower(username) = lower(?1) THEN 0 "
        "WHEN instr(lower(username), lower(?1)) = 1 THEN 1 ELSE 2 END, lower(username), id LIMIT 5",
        std::move(query));
    std::vector<UserMatch> result;
    for (const auto& row : rows) {
        result.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>()});
    }
    co_return result;
}

drogon::Task<bool> UserDao::insertAccount(std::string username, std::string passwordHash) const {
    const auto result = co_await executeSql(
        "INSERT INTO users (username, password_hash) VALUES (?, ?) "
        "ON CONFLICT(username) DO NOTHING", std::move(username), std::move(passwordHash));
    co_return result.affectedRows() == 1;
}

drogon::Task<bool> UserDao::hasDemo() const {
    co_return (co_await count(includeDeleted(), where<&UserRecord::seed_key>("demo"))) != 0;
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
