#pragma once

#include "app_namespace.h"

#include <glaze/glaze.hpp>

#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace APP_NAMESPACE {

namespace config_detail {

inline std::string_view trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

inline bool valid_environment_name(std::string_view name) {
    constexpr std::string_view characters =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";
    return !name.empty() && !(name.front() >= '0' && name.front() <= '9') &&
           name.find_first_not_of(characters) == std::string_view::npos;
}

inline std::string read_json_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

inline std::string environment_name(std::string_view key) {
    std::string name = "APP_";
    for (const char character : key) {
        if (character >= 'a' && character <= 'z') {
            name += static_cast<char>(character - 'a' + 'A');
        } else {
            name += character;
        }
    }
    return name;
}

template<class T>
void apply_environment(T& config) {
    std::size_t index = 0;
    glz::for_each_field(config, [&index](auto& field) {
        const auto name = environment_name(glz::reflect<T>::keys[index++]);
        const auto* configured = std::getenv(name.c_str());
        if (!configured) return;

        using Field = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::same_as<Field, std::string> ||
                      std::same_as<Field, std::optional<std::string>>) {
            field = std::string(configured);
        } else {
            const std::string_view json(configured);
            Field replacement{};
            if (glz::validate_json(json) || glz::read_json(replacement, json)) {
                throw std::runtime_error("invalid value for " + name);
            }
            field = std::move(replacement);
        }
    });
}

}

inline void load_environment(const std::filesystem::path& env_path) {
    if (!std::filesystem::exists(env_path)) return;
    std::ifstream input(env_path);
    if (!input) throw std::runtime_error("could not open .env");

    std::size_t line_number = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        const auto assignment = config_detail::trim(line);
        if (assignment.empty() || assignment.starts_with('#')) continue;

        const auto equal = assignment.find('=');
        const auto name = config_detail::trim(assignment.substr(0, equal));
        if (equal == std::string_view::npos || !config_detail::valid_environment_name(name) ||
            assignment.contains('\0')) {
            throw std::runtime_error("invalid .env assignment on line " + std::to_string(line_number));
        }

        auto value = config_detail::trim(assignment.substr(equal + 1));
        if (value.starts_with('"') || value.starts_with('\'')) {
            if (value.size() < 2 || value.back() != value.front()) {
                throw std::runtime_error("unclosed .env quote on line " + std::to_string(line_number));
            }
            value = value.substr(1, value.size() - 2);
        }

        if (setenv(std::string(name).c_str(), std::string(value).c_str(), 0) != 0) {
            throw std::runtime_error("could not load .env assignment on line " + std::to_string(line_number));
        }
    }
    if (input.bad()) throw std::runtime_error("could not read .env");
}

template<glz::reflectable T>
T read_config(const std::filesystem::path& path) {
    const auto json = config_detail::read_json_file(path);
    T config{};
    if (glz::validate_json(json) || glz::read_json(config, json)) {
        throw std::runtime_error("invalid config.json fields or JSON syntax");
    }

    config_detail::apply_environment(config);
    if constexpr (requires(const T& value) { { value.validate() } -> std::same_as<void>; }) {
        static_cast<const T&>(config).validate();
    }
    return config;
}

template<glz::reflectable T>
T load_config(const std::filesystem::path& root) {
    load_environment(root / ".env");
    return read_config<T>(root / "config.json");
}

}
