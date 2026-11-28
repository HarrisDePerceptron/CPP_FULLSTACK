#pragma once

#include "app_namespace.h"

#include <glaze/json/generic.hpp>
#include <magic_enum/magic_enum.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace APP_NAMESPACE {

std::string load_signing_key();

template <typename E>
concept RoleEnum = std::is_scoped_enum_v<E>;

template <RoleEnum E> std::string_view role_name(E role) {
    const auto name = magic_enum::enum_name(role);
    if (name.empty())
        throw std::invalid_argument("Unknown role enum value");
    return name;
}

struct VerifiedJwt {
    std::string subject;
    std::string issuer;
    std::chrono::system_clock::time_point expires_at;
    std::optional<std::chrono::system_clock::time_point> issued_at;
    glz::generic_i64::object_t header;
    glz::generic_i64::object_t claims;
    std::vector<std::string> role_names;

    template <RoleEnum E> std::vector<E> roles() const {
        std::vector<E> result;
        result.reserve(role_names.size());
        for (const auto& name : role_names) {
            if (auto value = magic_enum::enum_cast<E>(name))
                result.push_back(*value);
        }
        return result;
    }
};

class Tokens {
  public:
    Tokens(std::string key, std::chrono::seconds lifetime)
        : key_(std::move(key)), lifetime_(lifetime) {}

    std::string issue(std::string_view username,
                      std::span<const std::string> roles = {}) const;
    std::optional<VerifiedJwt> verify(std::string_view token) const;
    std::chrono::seconds lifetime() const { return lifetime_; }

  private:
    std::string key_;
    std::chrono::seconds lifetime_;
};

}
