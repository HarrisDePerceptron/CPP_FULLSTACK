#pragma once

#include "app/dao/user_dao.h"
#include "framework/support/workers.h"

#include <optional>
#include <string>
#include <utility>

namespace APP_NAMESPACE {

struct Credentials {
    std::string username;
    std::string password;
};

class UserService {
  public:
    explicit UserService(UserDao dao) : dao_(std::move(dao)) {}

    drogon::Task<std::optional<Credentials>> seed_demo() const;
    drogon::Task<> configure_demo(std::string username, std::string password) const;
    const UserDao& dao() const { return dao_; }

    drogon::Task<std::optional<UserRecord::Id>> find_id(std::string username) const;
    drogon::Task<bool> signup(Credentials credentials, Workers& workers) const;
    drogon::Task<bool> authenticate(std::string username, std::string password, Workers& workers) const;

  private:
    UserDao dao_;
};

}
