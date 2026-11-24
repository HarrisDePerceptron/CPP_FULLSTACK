#pragma once

#include "app_namespace.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace APP_NAMESPACE {

enum class LogLevel { info, warn, error };

struct Config {
    std::string bindAddress;
    int port;
    std::size_t maxConnections;
    std::chrono::seconds jwtLifetime;
    LogLevel logLevel;
    std::string testUser;
    std::string testPassword;
};

Config readConfig(const std::filesystem::path& path);
void loadEnvironment(const std::filesystem::path& envPath);
std::string loadSigningKey();

}
