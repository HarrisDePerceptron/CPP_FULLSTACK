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

class DatabaseBackend {
public:
    virtual ~DatabaseBackend() = default;
    virtual const drogon::orm::DbClientPtr& client() const = 0;
    virtual std::string parameter(std::size_t index) const = 0;
    virtual std::string containsText(std::string_view column, std::size_t index) const = 0;
    virtual std::string nullSafeEqual(std::string_view column, std::size_t index) const = 0;
    virtual drogon::Task<> initializeTable(
        std::string table, std::vector<DatabaseColumn> columns) = 0;
    virtual std::optional<std::reference_wrapper<drogon::Mutex>> queryGate() const { return std::nullopt; }
};

using DatabaseBackendPtr = std::shared_ptr<DatabaseBackend>;

drogon::Task<DatabaseBackendPtr> openSqliteDatabase(std::filesystem::path path);

namespace detail {

std::string quoteIdentifier(std::string_view identifier);

using QueryValue = std::variant<std::nullptr_t, std::int64_t, double, bool, std::string>;

template <typename Awaiter>
drogon::Task<drogon::orm::Result> awaitQuery(Awaiter query) {
    auto* caller = trantor::EventLoop::getEventLoopOfCurrentThread();
    std::optional<drogon::orm::Result> result;
    std::exception_ptr error;
    try {
        result.emplace(co_await query);
    } catch (...) {
        error = std::current_exception();
    }
    if (caller && !caller->isInLoopThread()) co_await drogon::switchThreadCoro(caller);
    if (error) std::rethrow_exception(error);
    co_return std::move(*result);
}

template <typename... Args>
drogon::Task<drogon::orm::Result> executeOn(
    const drogon::orm::DbClientPtr& client, std::string sql, Args... args) {
    co_return co_await awaitQuery(client->execSqlCoro(sql, args...));
}

inline drogon::Task<drogon::orm::Result> executeBoundOn(
    const drogon::orm::DbClientPtr& client, std::string sql, std::vector<QueryValue> values) {
    auto binder = *client << sql;
    for (const auto& value : values) {
        std::visit([&](const auto& argument) { binder << argument; }, value);
    }
    co_return co_await awaitQuery(drogon::orm::internal::SqlAwaiter(std::move(binder)));
}

}

}
