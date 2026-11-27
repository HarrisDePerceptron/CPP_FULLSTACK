#include "user_dao.h"

namespace APP_NAMESPACE {

drogon::Task<UserDao> UserDao::init(DatabaseBackendPtr database) {
    co_return co_await finish_initialization(UserDao(std::move(database)));
}

UserDao::UserDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}

drogon::Task<std::optional<UserRecord>>
UserDao::find_by_username(std::string username) const& {
    auto rows = co_await find()
                    .limit(1)
                    .select_all()
                    .where<&UserRecord::username>(std::move(username))
                    .collect();
    if (rows.items.empty())
        co_return std::nullopt;
    co_return std::move(rows.items.front());
}

drogon::Task<std::vector<UserMatch>> UserDao::search(std::string query) const& {
    const auto rows = co_await execute_sql(
        "SELECT id, username FROM users WHERE deleted_at IS NULL AND "
        "instr(lower(username), lower(?1)) > 0 "
        "ORDER BY CASE WHEN lower(username) = lower(?1) THEN 0 "
        "WHEN instr(lower(username), lower(?1)) = 1 THEN 1 ELSE 2 END, "
        "lower(username), id LIMIT 5",
        std::move(query));
    std::vector<UserMatch> result;
    for (const auto& row : rows) {
        result.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>()});
    }
    co_return result;
}

drogon::Task<bool> UserDao::insert_account(std::string username,
                                           std::string password_hash) const& {
    return insert_if_absent<&UserRecord::username>(
        {{std::move(username), std::move(password_hash)}});
}

drogon::Task<bool> UserDao::has_demo() const& {
    co_return (co_await count()
                   .include_deleted()
                   .where<&UserRecord::seed_key>("demo")
                   .collect()) != 0;
}

drogon::Task<bool> UserDao::insert_demo(std::string username,
                                        std::string password_hash) const& {
    return insert_if_absent<&UserRecord::seed_key>(
        {{std::move(username), std::move(password_hash), "demo"}});
}

drogon::Task<> UserDao::replace_demo(std::string username,
                                     std::string password_hash) const& {
    co_await upsert<&UserRecord::seed_key, &UserRecord::username,
                    &UserRecord::password_hash>(
        {{std::move(username), std::move(password_hash), "demo"}});
}

}
