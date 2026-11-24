#pragma once

#include "app_namespace.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

class UserDao {
public:
    explicit UserDao(std::filesystem::path database) : database_(std::move(database)) {}

    void initialize() const;
    bool hasDemo() const;
    bool insertDemo(std::string_view username, std::string_view passwordHash) const;
    void replaceDemo(std::string_view username, std::string_view passwordHash) const;
    std::optional<std::string> passwordHash(std::string_view username) const;

private:
    std::filesystem::path database_;
};

}
