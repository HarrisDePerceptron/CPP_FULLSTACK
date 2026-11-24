#pragma once

#include "database_backend.hpp"
#include "dao_record.hpp"
#include "row_to_struct.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <glaze/reflection/get_name.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <tuple>
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

struct DaoOptions { bool timestamps = true; };

template <typename Derived, typename Record, DaoOptions Options = {}>
class BaseDao {
    using Fields = typename Record::FieldRecord;
    using Id = typename Record::Id;

    template <auto Member>
    static constexpr bool recordMember =
        std::same_as<Fields, typename detail::DaoPredicate<Member>::Record> ||
        std::same_as<detail::DaoTimestamps<Fields>, typename detail::DaoPredicate<Member>::Record> ||
        (Record::generatedId &&
         std::same_as<detail::DaoGeneratedKey<Fields, true>, typename detail::DaoPredicate<Member>::Record>);

public:
    drogon::Task<std::vector<Record>> find(std::int64_t limit = 100, std::int64_t offset = 0) const {
        return findPage(limit, offset);
    }

    template <auto... Members>
        requires (sizeof...(Members) > 0)
            && (recordMember<Members> && ...)
    drogon::Task<std::vector<Record>> find(detail::DaoPredicate<Members>... predicates) const {
        return findPage(100, 0, std::move(predicates)...);
    }

    template <auto... Members>
        requires (sizeof...(Members) > 0)
            && (recordMember<Members> && ...)
    drogon::Task<std::vector<Record>> find(
        std::int64_t limit, std::int64_t offset, detail::DaoPredicate<Members>... predicates) const {
        return findPage(limit, offset, std::move(predicates)...);
    }

    drogon::Task<std::optional<Record>> findById(Id id) const {
        const auto rows = co_await executeSql(select_ + " WHERE " + key_ + " = " + parameter(1), std::move(id));
        if (rows.empty()) co_return std::nullopt;
        co_return mapRow(rows[0]);
    }

    drogon::Task<Id> create(Record record) const {
        checkId<true>(record);
        const auto result = co_await writeRecord<true>(insert_, record);
        co_return result[0][0].template as<Id>();
    }

    drogon::Task<bool> update(Record record) const {
        checkId<false>(record);
        const auto result = co_await writeRecord<false>(update_, record);
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

protected:
    static drogon::Task<Derived> finishInitialization(Derived dao) {
        auto& base = static_cast<BaseDao&>(dao);
        co_await base.database_->initializeTable(base.tableName_, std::move(base.schema_), base.migration_);
        co_return std::move(dao);
    }

    BaseDao(const BaseDao&) = delete;
    BaseDao& operator=(const BaseDao&) = delete;
    BaseDao(BaseDao&&) noexcept = default;
    BaseDao& operator=(BaseDao&&) noexcept = default;

    BaseDao(DatabaseBackendPtr database, std::string_view table,
            LegacyMigration migration = LegacyMigration::none)
        : database_(std::move(database)), tableName_(table), table_(detail::quoteIdentifier(table)),
          migration_(migration) {
        if (!database_) throw std::invalid_argument("DAO requires a database backend");
        buildSchema(indices_);
        std::string projection, columns, placeholders, assignments, keyParameter;
        std::size_t insertIndex = 0, updateIndex = 0;
        for (const auto& field : schema_) {
            const auto column = detail::quoteIdentifier(field.name);
            if (!projection.empty()) projection += ", ";
            projection += column;
            if (field.timestamp) continue;

            const auto marker = parameter(++updateIndex);
            if (field.primary) {
                key_ = column;
                keyParameter = marker;
            } else {
                if (!assignments.empty()) assignments += ", ";
                assignments += column + " = " + marker;
            }
            if (!field.generated) {
                if (!columns.empty()) {
                    columns += ", ";
                    placeholders += ", ";
                }
                columns += column;
                placeholders += parameter(++insertIndex);
            }
        }
        if (assignments.empty()) assignments = key_ + " = " + key_;
        update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + keyParameter;
        select_ = "SELECT " + projection + " FROM " + table_;
        insert_ = "INSERT INTO " + table_;
        if (columns.empty()) insert_ += " DEFAULT VALUES";
        else insert_ += " (" + columns + ") VALUES (" + placeholders + ")";
        insert_ += " RETURNING " + key_;
    }

    std::string parameter(std::size_t index) const { return database_->parameter(index); }

private:
    static constexpr std::array<std::string_view, 2> timestampNames_{"created_at", "updated_at"};
    static constexpr auto names_ = glz::reflect<Fields>::keys;
    static constexpr auto indices_ = std::make_index_sequence<names_.size()>{};

    Record mapRow(const drogon::orm::Row& row) const {
        Record result(rowToStruct<Fields>(row));
        if constexpr (Record::generatedId) result.id = row.at("id").template as<Id>();
        if constexpr (Options.timestamps) {
            result.created_at = readTimestamp(row.at("created_at"));
            result.updated_at = readTimestamp(row.at("updated_at"));
        }
        return result;
    }

    static std::optional<DaoDateTime> readTimestamp(const drogon::orm::Field& field) {
        if (field.isNull()) return std::nullopt;
        return detail::parseDaoDateTime(field.as<std::string>());
    }

    static constexpr bool timestampName(std::string_view name) {
        return std::ranges::find(timestampNames_, name) != timestampNames_.end();
    }

    template <std::size_t I>
    using Traits = detail::DaoFieldTraits<Fields, I>;

    template <std::size_t I>
    using Value = detail::Nullable<typename Traits<I>::Value>;

    template <std::size_t I>
    void addField() {
        constexpr auto name = names_[I];
        static_assert(!timestampName(name) && (!Record::generatedId || name != "id"),
                      "reserved DAO fields are inherited");
        schema_.push_back({std::string(name), detail::columnType<typename Value<I>::Type>(),
                           Value<I>::nullable, Traits<I>::primary, Traits<I>::unique, Traits<I>::indexed,
                           Traits<I>::defaultMetadata()});
    }

    template <std::size_t... I>
    void buildSchema(std::index_sequence<I...>) {
        (addField<I>(), ...);
        if constexpr (Record::generatedId) {
            schema_.push_back({"id", ColumnType::integer, false, true, false, false, {}, true});
        }
        if constexpr (Options.timestamps) {
            for (const auto name : timestampNames_) {
                schema_.push_back({std::string(name), ColumnType::datetime, true, false, false, false, {}, false, true});
            }
        }
    }

    template <bool Insert>
    static void checkId(const Record& record) {
        if constexpr (Record::generatedId) {
            if (record.id.has_value() == Insert) {
                throw std::invalid_argument(Insert ? "create requires an empty generated ID" :
                                             "update requires a generated ID");
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
            std::size_t index = 0;
            auto append = [&](std::string_view name) {
                if (!Options.timestamps && timestampName(name)) {
                    throw std::invalid_argument("timestamp filters require timestamps to be enabled");
                }
                sql += separator + database_->nullSafeEqual(detail::quoteIdentifier(name), ++index);
                separator = " AND ";
            };
            (append(detail::DaoPredicate<Members>::name), ...);
        }
        sql += " ORDER BY " + key_ + " LIMIT " + parameter(sizeof...(Members) + 1) +
               " OFFSET " + parameter(sizeof...(Members) + 2);
        const auto rows = co_await executeSql(std::move(sql), std::move(predicates.value)..., limit, offset);
        std::vector<Record> records;
        records.reserve(rows.size());
        for (const auto& row : rows) records.push_back(mapRow(row));
        co_return records;
    }

    template <bool Insert>
    drogon::Task<drogon::orm::Result> writeRecord(const std::string& sql, Record& record) const {
        auto fields = glz::to_tie(static_cast<Fields&>(record));
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            if constexpr (!Insert && Record::generatedId) {
                return executeSql(sql, Traits<I>::unwrap(glz::get<I>(fields))..., *record.id);
            } else {
                return executeSql(sql, Traits<I>::unwrap(glz::get<I>(fields))...);
            }
        }(indices_);
    }

    template <typename T>
    static auto ownArgument(T&& value) {
        if constexpr (std::same_as<std::remove_cvref_t<T>, DaoDateTime>) {
            return detail::formatDaoDateTime(value);
        } else if constexpr (std::same_as<std::remove_cvref_t<T>, std::optional<DaoDateTime>>) {
            return value ? std::optional{detail::formatDaoDateTime(*value)} : std::nullopt;
        } else if constexpr (std::is_convertible_v<T, std::string_view>
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

    DatabaseBackendPtr database_;
    std::string tableName_, table_, key_, select_, insert_, update_;
    std::vector<DatabaseColumn> schema_;
    LegacyMigration migration_;
};

}
