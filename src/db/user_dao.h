#pragma once

#include "app_namespace.h"

#include "base_dao.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

struct UserFields {
  static constexpr std::string_view table_name = "users";

  Unique<std::string> username;
  std::string password_hash;
  Unique<std::optional<std::string>> seed_key;
};

using UserRecord = DaoRecord<UserFields>;

struct UserMatch {
    std::int64_t id;
    std::string username;
};

class UserDao final : public BaseDao<UserDao, UserRecord> {
public:
  static drogon::Task<UserDao> init(DatabaseBackendPtr database);

  drogon::Task<std::optional<UserRecord>> find_by_username(std::string username) const &;
  drogon::Task<std::optional<UserRecord>> find_by_username(std::string username) const && = delete;
  drogon::Task<std::vector<UserMatch>> search(std::string query) const &;
  drogon::Task<std::vector<UserMatch>> search(std::string query) const && = delete;
  drogon::Task<bool> has_demo() const &;
  drogon::Task<bool> has_demo() const && = delete;
  drogon::Task<bool> insert_account(std::string username, std::string password_hash) const &;
  drogon::Task<bool> insert_account(std::string username, std::string password_hash) const && = delete;
  drogon::Task<bool> insert_demo(std::string username,
                                std::string password_hash) const &;
  drogon::Task<bool> insert_demo(std::string username,
                                std::string password_hash) const && = delete;
  drogon::Task<> replace_demo(std::string username,
                             std::string password_hash) const &;
  drogon::Task<> replace_demo(std::string username,
                             std::string password_hash) const && = delete;

private:
  explicit UserDao(DatabaseBackendPtr database);
};

} // namespace APP_NAMESPACE
