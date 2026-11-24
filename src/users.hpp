#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hello {

struct Credentials {
    std::string username;
    std::string password;
};

class Users {
public:
    explicit Users(std::filesystem::path database) : database_(std::move(database)) {}

    std::optional<Credentials> seedDemo() const;
    bool authenticate(std::string_view username, std::string_view password) const;

private:
    std::filesystem::path database_;
};

}
