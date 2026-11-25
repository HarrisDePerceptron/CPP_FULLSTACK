#pragma once

#include "app_namespace.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace APP_NAMESPACE {

enum class LogLevel { info, warn, error };

struct Config {
    std::string bind_address;
    int port;
    std::size_t max_connections;
    std::chrono::seconds jwt_lifetime;
    LogLevel log_level;
    std::string test_user;
    std::string test_password;
};

Config read_config(const std::filesystem::path& path);
void load_environment(const std::filesystem::path& env_path);
std::string load_signing_key();

}
