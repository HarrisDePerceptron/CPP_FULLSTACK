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
    std::string username;
    std::string passwordHash;
    std::optional<std::string> seedKey;
};

class UserDao : public BaseDao<UserRecord, std::string> {
public:
    explicit UserDao(std::filesystem::path database);

    drogon::Task<> initialize() const;
    drogon::Task<bool> hasDemo() const;
    drogon::Task<bool> insertDemo(std::string username, std::string passwordHash) const;
    drogon::Task<> replaceDemo(std::string username, std::string passwordHash) const;
};

}
