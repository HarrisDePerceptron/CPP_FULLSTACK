#pragma once

#include "app_namespace.h"

#include "framework/db/dao_record.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

enum class Role { member, admin };

struct UserFields {
    static constexpr std::string_view table_name = "users";

    Unique<std::string> username;
    std::string password_hash;
    Unique<std::optional<std::string>> seed_key;
    Default<std::string, SqlText{R"(["member"])"}> roles;
};

using UserRecord = DaoRecord<UserFields>;

struct UserMatch {
    std::int64_t id;
    std::string username;
};

}
