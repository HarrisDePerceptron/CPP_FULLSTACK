#pragma once

#include "app_namespace.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

class Tokens {
public:
    Tokens(std::string key, std::chrono::seconds lifetime)
        : key_(std::move(key)), lifetime_(lifetime) {}

    std::string issue(std::string_view username) const;
    std::optional<std::string> verify(std::string_view token) const;
    std::chrono::seconds lifetime() const { return lifetime_; }

private:
    std::string key_;
    std::chrono::seconds lifetime_;
};

}
