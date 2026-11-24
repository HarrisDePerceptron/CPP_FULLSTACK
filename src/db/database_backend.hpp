#pragma once

#include "schema_field.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace APP_NAMESPACE {

enum class LegacyMigration { none, usersUsernameKey };

class DatabaseBackend {
public:
    virtual ~DatabaseBackend() = default;
    virtual const drogon::orm::DbClientPtr& client() const = 0;
    virtual std::string parameter(std::size_t index) const = 0;
    virtual std::string nullSafeEqual(std::string_view column, std::size_t index) const = 0;
    virtual drogon::Task<> initializeTable(
        std::string table, std::vector<DatabaseColumn> columns, LegacyMigration migration) = 0;
    virtual std::optional<std::reference_wrapper<drogon::Mutex>> queryGate() const { return std::nullopt; }
};

using DatabaseBackendPtr = std::shared_ptr<DatabaseBackend>;

drogon::Task<DatabaseBackendPtr> openSqliteDatabase(std::filesystem::path path);

namespace detail {

std::string quoteIdentifier(std::string_view identifier);

template <typename... Args>
drogon::Task<drogon::orm::Result> executeOn(
    const drogon::orm::DbClientPtr& client, std::string sql, Args... args) {
    auto* caller = trantor::EventLoop::getEventLoopOfCurrentThread();
    std::optional<drogon::orm::Result> result;
    std::exception_ptr error;
    try {
        result.emplace(co_await client->execSqlCoro(sql, args...));
    } catch (...) {
        error = std::current_exception();
    }
    if (caller && !caller->isInLoopThread()) co_await drogon::switchThreadCoro(caller);
    if (error) std::rethrow_exception(error);
    co_return std::move(*result);
}

}

}
