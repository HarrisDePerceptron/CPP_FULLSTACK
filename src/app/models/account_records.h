#pragma once

#include "app_namespace.h"

#include "framework/db/dao_record.h"

#include <optional>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

struct UserFields {
    static constexpr std::string_view table_name = "users";

    Unique<std::string> username;
    std::string password_hash;
    Unique<std::optional<std::string>> seed_key;
};

using UserRecord = DaoRecord<UserFields>;

}
