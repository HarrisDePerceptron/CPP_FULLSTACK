#pragma once

#include "app/dao/records/user_records.h"

#include "framework/db/base_dao.h"

#include <optional>
#include <string>
#include <vector>

namespace APP_NAMESPACE {

class UserDao final : public BaseDao<UserDao, UserRecord> {
  public:
    static drogon::Task<UserDao> init(DatabaseBackendPtr database);

    drogon::Task<std::optional<UserRecord>>
    find_by_username(std::string username) const&;
    drogon::Task<std::optional<UserRecord>>
    find_by_username(std::string username) const&& = delete;
    drogon::Task<std::vector<UserMatch>> search(std::string query) const&;
    drogon::Task<std::vector<UserMatch>>
    search(std::string query) const&& = delete;
    drogon::Task<bool> has_demo() const&;
    drogon::Task<bool> has_demo() const&& = delete;
    drogon::Task<bool> insert_account(std::string username,
                                      std::string password_hash) const&;
    drogon::Task<bool>
    insert_account(std::string username,
                   std::string password_hash) const&& = delete;
    drogon::Task<bool> insert_demo(std::string username,
                                   std::string password_hash) const&;
    drogon::Task<bool> insert_demo(std::string username,
                                   std::string password_hash) const&& = delete;
    drogon::Task<> replace_demo(std::string username,
                                std::string password_hash) const&;
    drogon::Task<> replace_demo(std::string username,
                                std::string password_hash) const&& = delete;

  private:
    explicit UserDao(DatabaseBackendPtr database);
};

} // namespace APP_NAMESPACE
