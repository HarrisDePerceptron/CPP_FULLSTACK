#include "app_namespace.h"
#include "config.h"

#include <glaze/glaze.hpp>
#include <laserpants/dotenv/dotenv.h>
#include <sodium.h>

#include <arpa/inet.h>

#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace APP_NAMESPACE {

struct Settings {
    std::string bind_address = "127.0.0.1";
    int port{};
    std::size_t max_connections = 64;
    int jwt_lifetime_seconds{};
    std::string log_level;
    std::string test_user;
    std::string test_password;
};

namespace {

template<class Number>
Number positive_number(Number value, Number maximum, std::string_view name) {
    if (value < 1 || value > maximum) {
        throw std::runtime_error(std::string(name) + " must be between 1 and " +
                                 std::to_string(maximum));
    }
    return value;
}

template<class Number>
Number number_override(const char* name, Number value, Number maximum) {
    if (const char* configured = std::getenv(name)) {
        const std::string_view text(configured);
        const auto end = text.data() + text.size();
        const auto [ptr, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc{} || ptr != end) value = 0;
        return positive_number(value, maximum, name);
    }
    return value;
}

void validate_address(const std::string& address, std::string_view name) {
    in_addr parsed{};
    if (address.find('\0') != std::string::npos ||
        inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        throw std::runtime_error(std::string(name) + " must be an IPv4 address");
    }
}

LogLevel parse_log_level(std::string_view level) {
    if (level == "info") return LogLevel::info;
    if (level == "warn") return LogLevel::warn;
    if (level == "error") return LogLevel::error;
    throw std::runtime_error("config.json log_level must be info, warn, or error");
}

}

Config read_config(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());

    const std::string json{std::istreambuf_iterator<char>(input), {}};
    Settings settings;
    if (glz::validate_json(json) ||
        glz::read_json(settings, json)) {
        throw std::runtime_error("invalid config.json fields or JSON syntax");
    }
    constexpr auto max_connections = std::numeric_limits<std::size_t>::max();
    validate_address(settings.bind_address, "config.json bind_address");
    positive_number(settings.port, 65535, "config.json port");
    positive_number(settings.max_connections, max_connections, "config.json max_connections");
    if (settings.jwt_lifetime_seconds < 1 || settings.jwt_lifetime_seconds > 86400) {
        throw std::runtime_error("config.json jwt_lifetime_seconds must be between 1 and 86400");
    }
    if (const char* address = std::getenv("APP_BIND_ADDRESS")) {
        settings.bind_address = address;
        validate_address(settings.bind_address, "APP_BIND_ADDRESS");
    }
    if (settings.test_user.empty() != settings.test_password.empty() ||
        settings.test_user.size() > 128 || settings.test_password.size() > 1024) {
        throw std::runtime_error("config.json test_user and test_password must be configured together "
                                 "and limited to 128 and 1024 bytes respectively");
    }
    return {std::move(settings.bind_address), number_override("APP_PORT", settings.port, 65535),
            number_override("APP_MAX_CONNECTIONS", settings.max_connections, max_connections),
            std::chrono::seconds(settings.jwt_lifetime_seconds), parse_log_level(settings.log_level),
            std::move(settings.test_user), std::move(settings.test_password)};
}

void load_environment(const std::filesystem::path& env_path) {
    if (std::filesystem::exists(env_path)) {
        std::ifstream input(env_path);
        if (!input) throw std::runtime_error("could not open .env");
        dotenv::init(dotenv::Preserve, env_path.c_str());
    }
}

std::string load_signing_key() {
    std::array<unsigned char, 32> key;
    if (const auto* configured = std::getenv("APP_JWT_SECRET")) {
        const std::string_view hex(configured);
        if (hex.size() != key.size() * 2 ||
            sodium_hex2bin(key.data(), key.size(), hex.data(), hex.size(), nullptr,
                           nullptr, nullptr) != 0) {
            throw std::runtime_error("APP_JWT_SECRET must contain 64 hexadecimal characters");
        }
    } else {
        randombytes_buf(key.data(), key.size());
    }
    std::string result(reinterpret_cast<const char*>(key.data()), key.size());
    sodium_memzero(key.data(), key.size());
    return result;
}

}
