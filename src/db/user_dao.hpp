#pragma once

#include "app_namespace.hpp"

#include "base_dao.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

struct UserFields {
    Unique<std::string> username;
    std::string password_hash;
    Unique<std::optional<std::string>> seed_key;
};

using UserRecord = DaoRecord<UserFields>;

class UserDao final : public BaseDao<UserDao, UserRecord> {
public:
    static drogon::Task<UserDao> init(DatabaseBackendPtr database);

    drogon::Task<std::optional<UserRecord>> findByUsername(std::string username) const;
    drogon::Task<bool> hasDemo() const;
    drogon::Task<bool> insertDemo(std::string username, std::string password_hash) const;
    drogon::Task<> replaceDemo(std::string username, std::string password_hash) const;

private:
    explicit UserDao(DatabaseBackendPtr database);
};

}
