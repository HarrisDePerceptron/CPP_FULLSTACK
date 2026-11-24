#pragma once

#include "database_backend.hpp"
#include "row_to_struct.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <glaze/reflection/get_name.hpp>

#include <concepts>
#include <cstdint>
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

template <typename Derived, glz::reflectable Record, typename Id>
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
        const auto rows = co_await executeSql(select_ + " WHERE " + key_ + " = " + parameter(1), std::move(id));
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
        const auto result = co_await executeSql("DELETE FROM " + table_ + " WHERE " + key_ + " = " + parameter(1), std::move(id));
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

    static drogon::Task<Derived> init(DatabaseBackendPtr database) {
        if (!database) throw std::invalid_argument("DAO requires a database backend");
        Derived dao(std::move(database));
        auto& base = static_cast<BaseDao&>(dao);
        co_await base.database_->initializeTable(base.tableName_, std::move(base.schema_));
        co_return std::move(dao);
    }

private:
    friend Derived;

    BaseDao(const BaseDao&) = delete;
    BaseDao& operator=(const BaseDao&) = delete;
    BaseDao(BaseDao&&) noexcept = default;
    BaseDao& operator=(BaseDao&&) noexcept = default;

    BaseDao(DatabaseBackendPtr database, std::string_view table)
        : database_(std::move(database)), tableName_(table), table_(detail::quoteIdentifier(table)) {
        buildSchema(std::make_index_sequence<glz::reflect<Record>::keys.size()>{});
        std::string projection, columns, placeholders, assignments, keyParameter;
        constexpr auto members = glz::reflect<Record>::keys;
        for (std::size_t i = 0; i < schema_.size(); ++i) {
            const auto& field = schema_[i];
            if (i != 0) {
                projection += ", ";
                columns += ", ";
                placeholders += ", ";
            }
            const auto column = detail::quoteIdentifier(field.name);
            projection += column + " AS " + detail::quoteIdentifier(members[i]);
            columns += column;
            const auto marker = parameter(i + 1);
            placeholders += marker;
            if (field.primary) {
                key_ = column;
                keyParameter = marker;
            } else {
                if (!assignments.empty()) assignments += ", ";
                assignments += column + " = " + marker;
            }
        }
        if (assignments.empty()) assignments = key_ + " = " + key_;
        update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + keyParameter;
        select_ = "SELECT " + projection + " FROM " + table_;
        insert_ = "INSERT INTO " + table_ + " (" + columns + ") VALUES (" + placeholders + ")";
    }

    std::string parameter(std::size_t index) const { return database_->parameter(index); }

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
        (schema_.push_back({snakeCase(names[I]), detail::columnType<typename Value<I>::Type>(),
                           Value<I>::nullable, Traits<I>::primary, Traits<I>::unique, Traits<I>::indexed,
                           Traits<I>::defaultMetadata()}), ...);
    }

    template <auto... Members>
    drogon::Task<std::vector<Record>> findPage(
        std::int64_t limit, std::int64_t offset, detail::DaoPredicate<Members>... predicates) const {
        if (limit <= 0 || offset < 0) throw std::invalid_argument("invalid pagination");
        auto sql = select_;
        if constexpr (sizeof...(Members) > 0) {
            auto separator = " WHERE ";
            std::size_t index = 0;
            auto append = [&](std::string_view name) {
                sql += separator + database_->nullSafeEqual(detail::quoteIdentifier(snakeCase(name)), ++index);
                separator = " AND ";
            };
            (append(detail::DaoPredicate<Members>::name), ...);
        }
        sql += " ORDER BY " + key_ + " LIMIT " + parameter(sizeof...(Members) + 1) +
               " OFFSET " + parameter(sizeof...(Members) + 2);
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
        if (const auto gate = database_->queryGate()) {
            const auto lock = co_await gate->get().scoped_lock();
            co_return co_await detail::executeOn(database_->client(), std::move(sql), std::move(args)...);
        }
        co_return co_await detail::executeOn(database_->client(), std::move(sql), std::move(args)...);
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

    DatabaseBackendPtr database_;
    std::string tableName_, table_, key_, select_, insert_, update_;
    std::vector<DatabaseColumn> schema_;
};

}
