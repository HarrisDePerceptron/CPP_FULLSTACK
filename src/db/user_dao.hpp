#pragma once

#include "app_namespace.hpp"

#include "base_dao.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

struct UserRecord {
    Primary<std::string> username;
    std::string passwordHash;
    Unique<std::optional<std::string>> seedKey;
};

class UserDao final : public BaseDao<UserDao, UserRecord, std::string> {
public:
    drogon::Task<bool> hasDemo() const;
    drogon::Task<bool> insertDemo(std::string username, std::string passwordHash) const;
    drogon::Task<> replaceDemo(std::string username, std::string passwordHash) const;

private:
    friend class BaseDao<UserDao, UserRecord, std::string>;
    explicit UserDao(DatabaseBackendPtr database);
};

}
