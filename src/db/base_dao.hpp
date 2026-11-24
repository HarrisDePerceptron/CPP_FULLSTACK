#pragma once

#include "row_to_struct.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <vector>

namespace APP_NAMESPACE {

template <glz::reflectable Record, typename Id>
class BaseDao {
public:
    drogon::Task<std::vector<Record>> find(std::int64_t limit = 100, std::int64_t offset = 0) const {
        if (limit <= 0 || offset < 0) throw std::invalid_argument("invalid pagination");
        const auto rows = co_await executeSql(select_ + " ORDER BY " + key_ + " LIMIT ? OFFSET ?", limit, offset);
        std::vector<Record> records;
        records.reserve(rows.size());
        for (const auto& row : rows) records.push_back(rowToStruct<Record>(row));
        co_return records;
    }

    drogon::Task<std::optional<Record>> findById(Id id) const {
        const auto rows = co_await executeSql(select_ + " WHERE " + key_ + " = ?", std::move(id));
        if (rows.empty()) co_return std::nullopt;
        co_return rowToStruct<Record>(rows[0]);
    }

    drogon::Task<> create(Record record) const {
        co_await writeRecord(insert_, record);
    }

    drogon::Task<bool> update(Record record) const {
        const auto result = co_await writeRecord(update_, record);
        co_return result.affectedRows() != 0;
    }

    drogon::Task<bool> deleteById(Id id) const {
        const auto result = co_await executeSql("DELETE FROM " + table_ + " WHERE " + key_ + " = ?", std::move(id));
        co_return result.affectedRows() != 0;
    }

    drogon::Task<std::int64_t> count() const {
        const auto rows = co_await executeSql("SELECT COUNT(*) FROM " + table_);
        co_return rows[0][0].template as<std::int64_t>();
    }

    // Use application-owned SQL and pass runtime values as bound parameters.
    template <typename... Args>
    drogon::Task<drogon::orm::Result> executeSql(std::string sql, Args&&... args) const {
        return execute(std::move(sql), ownArgument(std::forward<Args>(args))...);
    }

protected:
    BaseDao(const std::filesystem::path& database, std::string_view table, std::string_view key)
        : table_(quoteIdentifier(table)), key_(quoteIdentifier(key)) {
        std::string connection = "filename='";
        for (char character : database.string()) {
            if (character == '\\' || character == '\'') connection += '\\';
            connection += character;
        }
        connection += '\'';
        db_ = drogon::orm::DbClient::newSqlite3Client(connection, 1);
        db_->setTimeout(5.0);

        std::string projection, columns, placeholders, assignments, keyParameter;
        std::size_t parameterIndex = 0;
        for (const auto name : glz::reflect<Record>::keys) {
            if (!columns.empty()) {
                projection += ", ";
                columns += ", ";
                placeholders += ", ";
            }
            const auto column = quoteIdentifier(snakeCase(name));
            projection += column + " AS " + quoteIdentifier(name);
            columns += column;
            placeholders += '?';
            const auto parameter = "?" + std::to_string(++parameterIndex);
            if (column == key_) {
                keyParameter = parameter;
            } else {
                if (!assignments.empty()) assignments += ", ";
                assignments += column + " = " + parameter;
            }
        }
        if (keyParameter.empty()) {
            throw std::invalid_argument("primary key must match a reflected record field");
        }
        if (assignments.empty()) assignments = key_ + " = " + key_;
        update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + keyParameter;
        select_ = "SELECT " + projection + " FROM " + table_;
        insert_ = "INSERT INTO " + table_ + " (" + columns + ") VALUES (" + placeholders + ")";
    }

    drogon::Task<> initializeConnection() const {
        co_await executeSql("PRAGMA busy_timeout = 3000");
    }

private:
    drogon::Task<drogon::orm::Result> writeRecord(const std::string& sql, Record& record) const {
        auto fields = glz::to_tie(record);
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return executeSql(sql, glz::get<I>(fields)...);
        }(std::make_index_sequence<glz::reflect<Record>::keys.size()>{});
    }

    template <typename T>
    static auto ownArgument(T&& value) {
        if constexpr (std::is_convertible_v<T, std::string_view>
                      && !std::is_same_v<std::decay_t<T>, std::nullptr_t>) {
            return std::string(std::forward<T>(value));
        } else {
            return std::decay_t<T>(std::forward<T>(value));
        }
    }

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute(std::string sql, Args... args) const {
        auto* caller = trantor::EventLoop::getEventLoopOfCurrentThread();
        std::optional<drogon::orm::Result> result;
        std::exception_ptr error;
        try {
            result.emplace(co_await db_->execSqlCoro(sql, args...));
        } catch (...) {
            error = std::current_exception();
        }
        if (caller && !caller->isInLoopThread()) co_await drogon::switchThreadCoro(caller);
        if (error) std::rethrow_exception(error);
        co_return std::move(*result);
    }

    static std::string quoteIdentifier(std::string_view identifier) {
        std::string quoted = "\"";
        for (const char character : identifier) {
            if (character == '"') quoted += '"';
            quoted += character;
        }
        return quoted + '"';
    }

    static std::string snakeCase(std::string_view name) {
        std::string column;
        for (const char character : name) {
            if (character >= 'A' && character <= 'Z') {
                if (!column.empty()) column += '_';
                column += static_cast<char>(character - 'A' + 'a');
            } else {
                column += character;
            }
        }
        return column;
    }

    drogon::orm::DbClientPtr db_;
    std::string table_, key_, select_, insert_, update_;
};

}
