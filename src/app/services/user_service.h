#pragma once

#include "app/dao/user_dao.h"
#include "password_hasher.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace APP_NAMESPACE {

struct Credentials {
    std::string username;
    std::string password;
};

class UserService {
  public:
    UserService(UserDao dao, PasswordHasher passwords)
        : dao_(std::move(dao)), passwords_(passwords) {}

    drogon::Task<std::optional<Credentials>> seed_demo() const;
    drogon::Task<> configure_demo(std::string username,
                                  std::string password) const;
    const UserDao& dao() const { return dao_; }

    drogon::Task<std::optional<UserRecord::Id>>
    find_id(std::string username) const;
    drogon::Task<bool> signup(Credentials credentials) const;
    drogon::Task<std::optional<std::vector<Role>>>
    authenticate(std::string username, std::string password) const;

  private:
    UserDao dao_;
    PasswordHasher passwords_;
};

}
