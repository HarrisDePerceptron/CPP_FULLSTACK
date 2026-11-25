#pragma once

#include "schema_field.h"

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

class DatabaseBackend;

class DaoTransaction {
public:
    virtual ~DaoTransaction() = default;
    DaoTransaction(const DaoTransaction&) = delete;
    DaoTransaction& operator=(const DaoTransaction&) = delete;

    virtual const DatabaseBackend& backend() const = 0;
    virtual const drogon::orm::DbClientPtr& client() const = 0;

protected:
    DaoTransaction() = default;
};

using TransactionCallback = std::function<drogon::Task<>(DaoTransaction&)>;

using BatchCallback = std::function<drogon::Task<>(const DaoTransaction&)>;

using CompositeUniqueness = std::vector<std::vector<std::string>>;

class DatabaseBackend {
public:
    virtual ~DatabaseBackend() = default;
    virtual const drogon::orm::DbClientPtr& client() const = 0;
    virtual std::string_view current_timestamp_sql() const = 0;
    virtual std::string parameter(std::size_t index) const = 0;
    virtual std::string contains_text(std::string_view column, std::size_t index) const = 0;
    virtual std::string null_safe_equal(std::string_view column, std::size_t index) const = 0;
    virtual drogon::Task<> transaction(TransactionCallback work) = 0;
    virtual drogon::Task<> savepoint(const DaoTransaction& tx, BatchCallback work) = 0;
    virtual drogon::Task<> initialize_table(
        std::string table, std::vector<DatabaseColumn> columns, CompositeUniqueness composite) = 0;
    virtual std::optional<std::reference_wrapper<drogon::Mutex>> query_gate() const { return std::nullopt; }
};

using DatabaseBackendPtr = std::shared_ptr<DatabaseBackend>;

drogon::Task<DatabaseBackendPtr> open_sqlite_database(std::filesystem::path path);

namespace detail {

std::string quote_identifier(std::string_view identifier);

using QueryValue = std::variant<std::nullptr_t, std::int64_t, double, bool, std::string>;

template <typename Awaiter>
drogon::Task<drogon::orm::Result> await_query(Awaiter query) {
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
drogon::Task<drogon::orm::Result> execute_on(
    const drogon::orm::DbClientPtr& client, std::string sql, Args... args) {
    co_return co_await await_query(client->execSqlCoro(sql, args...));
}

inline drogon::Task<drogon::orm::Result> execute_bound_on(
    const drogon::orm::DbClientPtr& client, std::string sql, std::vector<QueryValue> values) {
    auto binder = *client << sql;
    for (const auto& value : values) {
        std::visit([&](const auto& argument) { binder << argument; }, value);
    }
    co_return co_await await_query(drogon::orm::internal::SqlAwaiter(std::move(binder)));
}

}

}
