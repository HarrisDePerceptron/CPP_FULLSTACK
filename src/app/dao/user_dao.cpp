#include "user_dao.h"

#include <glaze/glaze.hpp>

#include <algorithm>

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

std::vector<Role> UserDao::read_roles(const UserRecord& user) {
    const auto& json = user.roles.value;
    std::vector<std::string> names;
    if (glz::validate_json(json) || glz::read_json(names, json))
        throw std::runtime_error("Invalid stored account roles");
    std::vector<Role> roles;
    roles.reserve(names.size());
    for (const auto& name : names) {
        const auto role = magic_enum::enum_cast<Role>(name);
        if (!role)
            throw std::runtime_error("Unknown stored account role");
        if (std::ranges::find(roles, *role) == roles.end())
            roles.push_back(*role);
    }
    return roles;
}

drogon::Task<bool> UserDao::set_roles(UserRecord::Id user,
                                      std::vector<Role> roles) const& {
    std::vector<std::string_view> names;
    names.reserve(roles.size());
    for (const auto role : roles)
        names.push_back(detail::enum_label(role));
    std::ranges::sort(names);
    const auto duplicates = std::ranges::unique(names);
    names.erase(duplicates.begin(), duplicates.end());
    auto json = glz::write_json(names);
    if (!json)
        throw std::runtime_error("Could not serialize account roles");
    return this->update_by_id(user)
        .set<&UserRecord::roles>(std::move(*json))
        .execute();
}

}
