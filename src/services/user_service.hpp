#pragma once

#include "app_namespace.hpp"

#include "db/user_dao.hpp"
#include "utils/workers.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

struct Credentials {
    std::string username;
    std::string password;
};

class UserService {
public:
    explicit UserService(UserDao dao) : dao_(std::move(dao)) {}

    drogon::Task<std::optional<Credentials>> seedDemo() const;
    drogon::Task<> configureDemo(std::string username, std::string password) const;
    drogon::Task<bool> authenticate(std::string username, std::string password, Workers& workers) const;

private:
    UserDao dao_;
};

}
