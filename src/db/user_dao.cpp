#include "user_dao.hpp"

namespace APP_NAMESPACE {

UserDao::UserDao(std::filesystem::path database) : BaseDao(database, "users", "username") {}

drogon::Task<> UserDao::initialize() const {
    co_await initializeConnection();
    co_await executeSql("CREATE TABLE IF NOT EXISTS users ("
                        "username TEXT PRIMARY KEY NOT NULL, password_hash TEXT NOT NULL, "
                        "seed_key TEXT UNIQUE)");
}

drogon::Task<bool> UserDao::hasDemo() const {
    const auto rows = co_await executeSql("SELECT 1 FROM users WHERE seed_key = 'demo'");
    co_return !rows.empty();
}

drogon::Task<bool> UserDao::insertDemo(std::string username, std::string passwordHash) const {
    const auto result = co_await executeSql(
        "INSERT INTO users (username, password_hash, seed_key) "
        "VALUES (?, ?, 'demo') ON CONFLICT(seed_key) DO NOTHING",
        std::move(username), std::move(passwordHash));
    co_return result.affectedRows() == 1;
}

drogon::Task<> UserDao::replaceDemo(std::string username, std::string passwordHash) const {
    co_await executeSql("INSERT INTO users (username, password_hash, seed_key) "
                        "VALUES (?, ?, 'demo') ON CONFLICT(seed_key) DO UPDATE SET "
                        "username = excluded.username, password_hash = excluded.password_hash",
                        std::move(username), std::move(passwordHash));
}

}
