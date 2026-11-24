#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace hello {

enum class LogLevel { info, warn, error };

struct Config {
    int port;
    std::chrono::seconds jwtLifetime;
    LogLevel logLevel;
};

Config readConfig(const std::filesystem::path& path);
std::string loadSigningKey(const std::filesystem::path& envPath);

}
