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
    explicit UserService(std::filesystem::path database) : dao_(std::move(database)) {}

    std::optional<Credentials> seedDemo() const;
    void configureDemo(std::string_view username, std::string_view password) const;
    drogon::Task<bool> authenticate(std::string username, std::string password, Workers& workers) const;

private:
    UserDao dao_;
};

}
