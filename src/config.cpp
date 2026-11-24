#include "config.hpp"

#include <glaze/glaze.hpp>
#include <laserpants/dotenv/dotenv.h>
#include <sodium.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace hello {

struct Settings {
    int port{};
    int jwt_lifetime_seconds{};
    std::string log_level;
};

namespace {

LogLevel parseLogLevel(std::string_view level) {
    if (level == "info") return LogLevel::info;
    if (level == "warn") return LogLevel::warn;
    if (level == "error") return LogLevel::error;
    throw std::runtime_error("config.json log_level must be info, warn, or error");
}

}

Config readConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());

    const std::string json{std::istreambuf_iterator<char>(input), {}};
    Settings settings;
    if (glz::validate_json(json) ||
        glz::read<glz::opts{.error_on_missing_keys = true}>(settings, json)) {
        throw std::runtime_error("invalid config.json fields or JSON syntax");
    }
    if (settings.port < 1 || settings.port > 65535) {
        throw std::runtime_error("config.json port must be between 1 and 65535");
    }
    if (settings.jwt_lifetime_seconds < 1 || settings.jwt_lifetime_seconds > 86400) {
        throw std::runtime_error("config.json jwt_lifetime_seconds must be between 1 and 86400");
    }
    return {settings.port, std::chrono::seconds(settings.jwt_lifetime_seconds),
            parseLogLevel(settings.log_level)};
}

std::string loadSigningKey(const std::filesystem::path& envPath) {
    if (std::filesystem::exists(envPath)) {
        std::ifstream input(envPath);
        if (!input) throw std::runtime_error("could not open .env");
        dotenv::init(dotenv::Preserve, envPath.c_str());
    }

    std::array<unsigned char, 32> key;
    if (const auto* configured = std::getenv("HELLO_JWT_SECRET")) {
        const std::string_view hex(configured);
        if (hex.size() != key.size() * 2 ||
            sodium_hex2bin(key.data(), key.size(), hex.data(), hex.size(), nullptr,
                           nullptr, nullptr) != 0) {
            throw std::runtime_error("HELLO_JWT_SECRET must contain 64 hexadecimal characters");
        }
    } else {
        randombytes_buf(key.data(), key.size());
    }
    std::string result(reinterpret_cast<const char*>(key.data()), key.size());
    sodium_memzero(key.data(), key.size());
    return result;
}

}
