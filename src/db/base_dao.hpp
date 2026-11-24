#pragma once

#include "row_to_struct.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <glaze/reflection/get_name.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <vector>

namespace APP_NAMESPACE {

namespace detail {

template <typename MemberPointer>
struct DaoMember;

template <typename Owner, typename Value>
struct DaoMember<Value Owner::*> {
    using Record = Owner;
    using Type = typename FieldTraits<std::remove_cv_t<Value>>::Value;
};

template <auto Member>
struct DaoPredicate {
    using Record = typename DaoMember<decltype(Member)>::Record;
    static constexpr auto name = glz::get_name<Member>();
    typename DaoMember<decltype(Member)>::Type value;
};

}

template <auto Member, typename Value>
    requires std::is_member_object_pointer_v<decltype(Member)>
        && std::constructible_from<typename detail::DaoMember<decltype(Member)>::Type, Value>
auto where(Value&& value) {
    using Type = typename detail::DaoMember<decltype(Member)>::Type;
    return detail::DaoPredicate<Member>{Type(std::forward<Value>(value))};
}

template <glz::reflectable Record, typename Id>
class BaseDao {
public:
    drogon::Task<std::vector<Record>> find(std::int64_t limit = 100, std::int64_t offset = 0) const {
        return findPage(limit, offset);
    }

    template <auto... Members>
        requires (sizeof...(Members) > 0)
            && (std::same_as<Record, typename detail::DaoPredicate<Members>::Record> && ...)
    drogon::Task<std::vector<Record>> find(detail::DaoPredicate<Members>... predicates) const {
        return findPage(100, 0, std::move(predicates)...);
    }

    template <auto... Members>
        requires (sizeof...(Members) > 0)
            && (std::same_as<Record, typename detail::DaoPredicate<Members>::Record> && ...)
    drogon::Task<std::vector<Record>> find(
        std::int64_t limit, std::int64_t offset, detail::DaoPredicate<Members>... predicates) const {
        return findPage(limit, offset, std::move(predicates)...);
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

    drogon::Task<> initialize() const {
        co_await executeSql("PRAGMA busy_timeout = 3000");
        co_await executeSql(createTable_);
        co_await validateSchema();
        for (const auto& column : schema_) {
            if (!column.indexed) continue;
            const auto index = "idx_" + std::to_string(tableName_.size()) + "_" + tableName_ + "_" + column.name;
            co_await executeSql("CREATE INDEX IF NOT EXISTS " + quoteIdentifier(index) +
                                " ON " + table_ + " (" + quoteIdentifier(column.name) + ")");
            const auto indexes = co_await executeSql("SELECT [unique], partial FROM pragma_index_list(?) WHERE name = ?", tableName_, index);
            const auto fields = co_await executeSql("SELECT name FROM pragma_index_info(?)", index);
            if (indexes.size() != 1 || indexes[0][0].template as<int>() != 0 ||
                indexes[0][1].template as<int>() != 0 || fields.size() != 1 ||
                fields[0][0].template as<std::string>() != column.name) {
                schemaError("invalid index " + index);
            }
        }
    }

protected:
    BaseDao(const std::filesystem::path& database, std::string_view table)
        : tableName_(table), table_(quoteIdentifier(table)) {
        std::string connection = "filename='";
        for (char character : database.string()) {
            if (character == '\\' || character == '\'') connection += '\\';
            connection += character;
        }
        connection += '\'';
        db_ = drogon::orm::DbClient::newSqlite3Client(connection, 1);
        db_->setTimeout(5.0);

        buildSchema(std::make_index_sequence<glz::reflect<Record>::keys.size()>{});
        std::string projection, columns, placeholders, assignments, keyParameter, definitions;
        std::size_t parameterIndex = 0;
        for (const auto& field : schema_) {
            if (!columns.empty()) {
                projection += ", ";
                columns += ", ";
                placeholders += ", ";
                definitions += ", ";
            }
            const auto column = quoteIdentifier(field.name);
            definitions += column + " " + std::string(field.type);
            if (field.primary) definitions += " PRIMARY KEY";
            if (!field.nullable) definitions += " NOT NULL";
            if (field.unique) definitions += " UNIQUE";
            projection += column + " AS " + quoteIdentifier(field.member);
            columns += column;
            placeholders += '?';
            const auto parameter = "?" + std::to_string(++parameterIndex);
            if (field.primary) {
                key_ = column;
                keyParameter = parameter;
            } else {
                if (!assignments.empty()) assignments += ", ";
                assignments += column + " = " + parameter;
            }
        }
        createTable_ = "CREATE TABLE IF NOT EXISTS " + table_ + " (" + definitions + ")";
        if (assignments.empty()) assignments = key_ + " = " + key_;
        update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + keyParameter;
        select_ = "SELECT " + projection + " FROM " + table_;
        insert_ = "INSERT INTO " + table_ + " (" + columns + ") VALUES (" + placeholders + ")";
    }

private:
    struct Column {
        std::string name;
        std::string_view member, type;
        bool nullable, primary, unique, indexed;
    };

    template <std::size_t I>
    using Traits = detail::FieldTraits<std::remove_cvref_t<decltype(
        glz::get<I>(glz::to_tie(std::declval<Record&>())))>>;

    template <std::size_t I>
    using Value = detail::Nullable<typename Traits<I>::Value>;

    template <std::size_t... I>
    void buildSchema(std::index_sequence<I...>) {
        static_assert((0 + ... + int(Traits<I>::primary)) == 1, "exactly one primary field is required");
        static_assert(((!Traits<I>::primary ||
            (!Value<I>::nullable && std::same_as<typename Value<I>::Type, Id>)) && ...),
            "primary field must be nonnullable and match the DAO ID type");
        constexpr auto names = glz::reflect<Record>::keys;
        (schema_.push_back({snakeCase(names[I]), names[I], detail::sqliteType<typename Value<I>::Type>(),
                           Value<I>::nullable, Traits<I>::primary, Traits<I>::unique, Traits<I>::indexed}), ...);
    }

    [[noreturn]] void schemaError(const std::string& detail) const {
        throw std::runtime_error("Schema mismatch for " + tableName_ + ": " + detail + "; explicit migration required");
    }

    drogon::Task<> validateSchema() const {
        const auto columns = co_await executeSql("SELECT name, upper(type), [notnull], pk FROM pragma_table_info(?)", tableName_);
        if (columns.size() != schema_.size()) schemaError("column count differs");
        const auto uniqueColumns = co_await executeSql(
            "SELECT info.name FROM pragma_index_list(?) AS idx "
            "JOIN pragma_index_info(idx.name) AS info "
            "WHERE idx.[unique] = 1 AND idx.origin != 'pk' AND idx.partial = 0 "
            "AND (SELECT count(*) FROM pragma_index_info(idx.name)) = 1", tableName_);
        for (const auto& expected : schema_) {
            auto actual = std::find_if(columns.begin(), columns.end(), [&](const auto& row) {
                return row[0].template as<std::string>() == expected.name;
            });
            const bool unique = std::any_of(uniqueColumns.begin(), uniqueColumns.end(), [&](const auto& row) {
                return !row[0].isNull() && row[0].template as<std::string>() == expected.name;
            });
            if (actual == columns.end() || (*actual)[1].template as<std::string>() != expected.type ||
                (*actual)[2].template as<int>() != !expected.nullable ||
                (*actual)[3].template as<int>() != expected.primary || unique != expected.unique) {
                schemaError("column " + expected.name);
            }
        }
    }

    template <auto... Members>
    drogon::Task<std::vector<Record>> findPage(
        std::int64_t limit, std::int64_t offset, detail::DaoPredicate<Members>... predicates) const {
        if (limit <= 0 || offset < 0) throw std::invalid_argument("invalid pagination");
        auto sql = select_;
        if constexpr (sizeof...(Members) > 0) {
            auto separator = " WHERE ";
            auto append = [&](std::string_view name) {
                sql += separator + quoteIdentifier(snakeCase(name)) + " IS ?";
                separator = " AND ";
            };
            (append(detail::DaoPredicate<Members>::name), ...);
        }
        sql += " ORDER BY " + key_ + " LIMIT ? OFFSET ?";
        const auto rows = co_await executeSql(std::move(sql), std::move(predicates.value)..., limit, offset);
        std::vector<Record> records;
        records.reserve(rows.size());
        for (const auto& row : rows) records.push_back(rowToStruct<Record>(row));
        co_return records;
    }

    drogon::Task<drogon::orm::Result> writeRecord(const std::string& sql, Record& record) const {
        auto fields = glz::to_tie(record);
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return executeSql(sql, Traits<I>::unwrap(glz::get<I>(fields))...);
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
    std::string tableName_, table_, key_, select_, insert_, update_, createTable_;
    std::vector<Column> schema_;
};

}
