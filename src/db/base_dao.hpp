#pragma once

#include "database_backend.hpp"
#include "dao_record.hpp"
#include "dao_predicate.hpp"
#include "row_to_struct.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <glaze/reflection/get_name.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <cstdint>
#include <vector>

namespace APP_NAMESPACE {

enum class SortDirection { asc, desc };

namespace detail {

template <auto... Members>
struct SelectFields {};

struct SelectAll {};
struct IncludeDeleted {};

template <auto Member>
struct OrderBy {
    SortDirection direction;
};

template <auto First, auto... Rest>
consteval bool validCompositeMembers() {
    using Owner = typename DaoMember<decltype(First)>::Record;
    if constexpr (!(std::same_as<Owner, typename DaoMember<decltype(Rest)>::Record> && ...)) {
        return false;
    } else {
        constexpr std::array names{glz::get_name<First>(), glz::get_name<Rest>()...};
        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = i + 1; j < names.size(); ++j) {
                if (names[i] == names[j]) return false;
            }
        }
        return true;
    }
}

}

inline auto includeDeleted() { return detail::IncludeDeleted{}; }

template <auto... Members>
    requires (sizeof...(Members) >= 2) &&
             (std::is_member_object_pointer_v<decltype(Members)> && ...) &&
             (detail::validCompositeMembers<Members...>())
struct CompositeUnique {};

struct DaoOptions { bool timestamps = true; };

template <typename Derived, typename Record, DaoOptions Options = {}>
class BaseDao {
    template <typename, typename, DaoOptions>
    friend class BaseDao;

    using Fields = typename Record::FieldRecord;
    using Id = typename Record::Id;
    using TransactionScope = std::optional<std::reference_wrapper<const DaoTransaction>>;

    template <auto Member>
    static constexpr bool recordMember =
        std::same_as<detail::DaoDeletion, typename detail::DaoPredicate<Member>::Record> ||
        std::same_as<Fields, typename detail::DaoPredicate<Member>::Record> ||
        std::same_as<detail::DaoTimestamps<Fields>, typename detail::DaoPredicate<Member>::Record> ||
        (Record::generatedId &&
         std::same_as<detail::DaoGeneratedKey<Fields, true>, typename detail::DaoPredicate<Member>::Record>);

    static constexpr bool validFilter(...) { return false; }

    template <auto Member>
    static constexpr bool validFilter(std::type_identity<detail::DaoPredicate<Member>>) {
        return recordMember<Member>;
    }

    template <auto First, auto Second>
    static constexpr bool validFilter(std::type_identity<detail::DaoTupleLess<First, Second>>) {
        return recordMember<First> && recordMember<Second>;
    }

public:
    using RecordType = Record;

    class EqualityFilters {
    public:
        template <auto Member, typename Value>
            requires std::is_member_object_pointer_v<decltype(Member)> &&
                     (recordMember<Member>) && requires(Value value) { where<Member>(value); }
        void equal(Value value) {
            const auto predicate = where<Member>(std::move(value));
            using FieldValue = typename detail::DaoPredicate<Member>::Value;
            entries_.emplace_back(predicate.name, queryValue(static_cast<FieldValue>(predicate.values.front())));
        }

    private:
        friend BaseDao;
        std::vector<std::pair<std::string_view, detail::QueryValue>> entries_;
    };

    template <typename... Modifiers>
    class FindBuilder {
    public:
        FindBuilder limit(std::int64_t value) && {
            limit_ = value;
            return std::move(*this);
        }

        FindBuilder offset(std::int64_t value) && {
            offset_ = value;
            return std::move(*this);
        }

        template <auto Member, typename Value>
            requires requires(Value&& value) { APP_NAMESPACE::where<Member>(std::forward<Value>(value)); } &&
                     recordMember<Member>
        auto where(Value&& value) && {
            return std::move(*this).where(APP_NAMESPACE::where<Member>(std::forward<Value>(value)));
        }

        template <typename Predicate>
            requires (validFilter(std::type_identity<Predicate>{}))
        auto where(Predicate predicate) && {
            return std::move(*this).append(std::move(predicate));
        }

        template <auto... Members>
            requires (sizeof...(Members) > 0) &&
                     (std::is_member_object_pointer_v<decltype(Members)> && ...) &&
                     (recordMember<Members> && ...)
        auto select() && {
            return std::move(*this).append(detail::SelectFields<Members...>{});
        }

        auto selectAll() && { return std::move(*this).append(detail::SelectAll{}); }

        template <auto Member>
            requires std::is_member_object_pointer_v<decltype(Member)> && recordMember<Member>
        auto orderBy(SortDirection direction = SortDirection::asc) && {
            return std::move(*this).append(detail::OrderBy<Member>{direction});
        }

        auto includeDeleted() && { return std::move(*this).append(detail::IncludeDeleted{}); }

        template <auto Member, typename Dao>
            requires detail::ForeignSelectionFor<Member, Record, std::remove_cv_t<Dao>>
        auto withForeign(Dao& dao) && {
            return std::move(*this).append(APP_NAMESPACE::withForeign<Member>(dao));
        }

        auto matching(EqualityFilters filters) && {
            return std::move(*this).append(std::move(filters));
        }

        drogon::Task<std::vector<Record>> collect() && {
            return std::apply([&](auto&&... modifiers) {
                return dao_.findPage(tx_, limit_, offset_, std::move(modifiers)...);
            }, std::move(modifiers_));
        }

    private:
        friend BaseDao;
        template <typename...>
        friend class FindBuilder;

        FindBuilder(const BaseDao& dao, TransactionScope tx, std::int64_t limit,
                    std::int64_t offset, std::tuple<Modifiers...> modifiers)
            : dao_(dao), tx_(tx), limit_(limit), offset_(offset), modifiers_(std::move(modifiers)) {}

        template <typename Modifier>
        auto append(Modifier modifier) && {
            return FindBuilder<Modifiers..., Modifier>(dao_, tx_, limit_, offset_,
                std::tuple_cat(std::move(modifiers_), std::tuple{std::move(modifier)}));
        }

        const BaseDao& dao_;
        TransactionScope tx_;
        std::int64_t limit_;
        std::int64_t offset_;
        std::tuple<Modifiers...> modifiers_;
    };

    FindBuilder<> find() const & { return FindBuilder<>(*this, {}, 100, 0, {}); }
    FindBuilder<> find(const DaoTransaction& tx) const & {
        return FindBuilder<>(*this, std::cref(tx), 100, 0, {});
    }
    FindBuilder<> find() const && = delete;
    FindBuilder<> find(const DaoTransaction&) const && = delete;

    drogon::Task<> transaction(TransactionCallback work) const {
        return database_->transaction(std::move(work));
    }

    template <bool Nullable>
    drogon::Task<bool> loadForeign(Foreign<Record, Nullable>& field) const {
        return field.load(*this);
    }

    template <bool Nullable>
    drogon::Task<bool> loadForeign(const DaoTransaction& tx, Foreign<Record, Nullable>& field) const {
        transactionClient(tx);
        return field.load(*this, tx);
    }

    template <typename... Selections>
    class FindByIdBuilder {
    public:
        FindByIdBuilder includeDeleted() && {
            include_ = true;
            return std::move(*this);
        }

        template <auto Member, typename Dao>
            requires detail::ForeignSelectionFor<Member, Record, std::remove_cv_t<Dao>>
        auto withForeign(Dao& dao) && {
            auto selection = APP_NAMESPACE::withForeign<Member>(dao);
            return FindByIdBuilder<Selections..., decltype(selection)>(dao_, tx_, std::move(id_),
                include_, std::tuple_cat(std::move(selections_), std::tuple{selection}));
        }

        drogon::Task<std::optional<Record>> collect() && {
            return std::apply([&](auto... selections) {
                return dao_.readWithForeign(tx_, std::move(id_), include_, selections...);
            }, std::move(selections_));
        }

    private:
        friend BaseDao;
        template <typename...>
        friend class FindByIdBuilder;

        FindByIdBuilder(const BaseDao& dao, TransactionScope tx, Id id, bool include,
                        std::tuple<Selections...> selections)
            : dao_(dao), tx_(tx), id_(std::move(id)), include_(include), selections_(std::move(selections)) {}

        const BaseDao& dao_;
        TransactionScope tx_;
        Id id_;
        bool include_;
        std::tuple<Selections...> selections_;
    };

    FindByIdBuilder<> findById(Id id) const & {
        return FindByIdBuilder<>(*this, {}, std::move(id), false, {});
    }

    FindByIdBuilder<> findById(const DaoTransaction& tx, Id id) const & {
        return FindByIdBuilder<>(*this, std::cref(tx), std::move(id), false, {});
    }

    FindByIdBuilder<> findById(Id) const && = delete;
    FindByIdBuilder<> findById(const DaoTransaction&, Id) const && = delete;

    bool usesBackend(const DatabaseBackendPtr& database) const {
        return database_ == database;
    }

    drogon::Task<Id> create(Record record) const {
        return createRecord({}, std::move(record));
    }

    drogon::Task<Id> create(const DaoTransaction& tx, Record record) const {
        return createRecord(std::cref(tx), std::move(record));
    }

    drogon::Task<bool> update(Record record) const {
        return updateRecord({}, std::move(record));
    }

    drogon::Task<bool> update(const DaoTransaction& tx, Record record) const {
        return updateRecord(std::cref(tx), std::move(record));
    }

    drogon::Task<std::vector<Id>> createMany(std::vector<Record> records) const {
        return createBatch({}, std::move(records));
    }

    drogon::Task<std::vector<Id>> createMany(const DaoTransaction& tx, std::vector<Record> records) const {
        return createBatch(std::cref(tx), std::move(records));
    }

    drogon::Task<bool> updateMany(std::vector<Record> records) const {
        return updateBatch({}, std::move(records));
    }

    drogon::Task<bool> updateMany(const DaoTransaction& tx, std::vector<Record> records) const {
        return updateBatch(std::cref(tx), std::move(records));
    }

    drogon::Task<bool> deleteById(Id id) const {
        return deleteRecord({}, std::move(id));
    }

    drogon::Task<bool> deleteById(const DaoTransaction& tx, Id id) const {
        return deleteRecord(std::cref(tx), std::move(id));
    }

    drogon::Task<bool> softDeleteById(Id id) const {
        return setDeleted({}, std::move(id), true);
    }

    drogon::Task<bool> softDeleteById(const DaoTransaction& tx, Id id) const {
        return setDeleted(std::cref(tx), std::move(id), true);
    }

    drogon::Task<bool> restoreById(Id id) const {
        return setDeleted({}, std::move(id), false);
    }

    drogon::Task<bool> restoreById(const DaoTransaction& tx, Id id) const {
        return setDeleted(std::cref(tx), std::move(id), false);
    }

    template <typename... Predicates>
    class CountBuilder {
    public:
        CountBuilder includeDeleted() && {
            include_ = true;
            return std::move(*this);
        }

        template <auto Member, typename Value>
            requires requires(Value&& value) { APP_NAMESPACE::where<Member>(std::forward<Value>(value)); } &&
                     recordMember<Member>
        auto where(Value&& value) && {
            return std::move(*this).where(APP_NAMESPACE::where<Member>(std::forward<Value>(value)));
        }

        template <typename Predicate>
            requires (validFilter(std::type_identity<Predicate>{}))
        auto where(Predicate predicate) && {
            return CountBuilder<Predicates..., Predicate>(dao_, tx_, include_,
                std::tuple_cat(std::move(predicates_), std::tuple{std::move(predicate)}));
        }

        drogon::Task<std::int64_t> collect() && {
            return std::apply([&](auto&&... predicates) {
                return dao_.countRecords(tx_, include_, std::move(predicates)...);
            }, std::move(predicates_));
        }

    private:
        friend BaseDao;
        template <typename...>
        friend class CountBuilder;

        CountBuilder(const BaseDao& dao, TransactionScope tx, bool include, std::tuple<Predicates...> predicates)
            : dao_(dao), tx_(tx), include_(include), predicates_(std::move(predicates)) {}

        const BaseDao& dao_;
        TransactionScope tx_;
        bool include_;
        std::tuple<Predicates...> predicates_;
    };

    CountBuilder<> count() const & { return CountBuilder<>(*this, {}, false, {}); }
    CountBuilder<> count(const DaoTransaction& tx) const & {
        return CountBuilder<>(*this, std::cref(tx), false, {});
    }
    CountBuilder<> count() const && = delete;
    CountBuilder<> count(const DaoTransaction&) const && = delete;

    // Use application-owned SQL and pass runtime values as bound parameters.
    template <typename... Args>
    drogon::Task<drogon::orm::Result> executeSql(std::string sql, Args&&... args) const {
        return execute({}, std::move(sql), ownArgument(std::forward<Args>(args))...);
    }

    template <typename... Args>
    drogon::Task<drogon::orm::Result> executeSql(const DaoTransaction& tx, std::string sql, Args&&... args) const {
        return execute(std::cref(tx), std::move(sql), ownArgument(std::forward<Args>(args))...);
    }

protected:
    static drogon::Task<Derived> finishInitialization(Derived dao) {
        auto& base = static_cast<BaseDao&>(dao);
        co_await base.database_->initializeTable(base.tableName_, std::move(base.schema_), compositeUniqueness());
        co_return std::move(dao);
    }

    BaseDao(const BaseDao&) = delete;
    BaseDao& operator=(const BaseDao&) = delete;
    BaseDao(BaseDao&&) noexcept = default;
    BaseDao& operator=(BaseDao&&) noexcept = default;

    explicit BaseDao(DatabaseBackendPtr database)
        : database_(std::move(database)), tableName_(Fields::table_name), table_(detail::quoteIdentifier(tableName_)) {
        if (!database_) throw std::invalid_argument("DAO requires a database backend");
        buildSchema(indices_);
        std::string projection, columns, placeholders, assignments, keyParameter;
        std::size_t insertIndex = 0, updateIndex = 0;
        std::size_t flagIndex = names_.size() + Record::generatedId;
        for (const auto& field : schema_) {
            const auto column = detail::quoteIdentifier(field.name);
            if (!projection.empty()) projection += ", ";
            projection += column;
            if (field.nullable) optionalColumns_.push_back(field.name);
            else appendColumn(requiredProjection_, column);
            if (field.timestamp || field.name == "deleted_at") continue;

            const auto marker = parameter(++updateIndex);
            if (field.primary) {
                key_ = column;
                keyParameter = marker;
            } else {
                if (!assignments.empty()) assignments += ", ";
                assignments += column + " = ";
                if (field.nullable) {
                    assignments += "CASE WHEN " + parameter(++flagIndex) + " THEN " + marker +
                                   " ELSE " + column + " END";
                } else {
                    assignments += marker;
                }
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
        update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + keyParameter + " AND deleted_at IS NULL";
        select_ = "SELECT " + projection + " FROM " + table_;
        insert_ = "INSERT INTO " + table_;
        if (columns.empty()) insert_ += " DEFAULT VALUES";
        else insert_ += " (" + columns + ") VALUES (" + placeholders + ")";
        insert_ += " RETURNING " + key_;
    }

    std::string parameter(std::size_t index) const { return database_->parameter(index); }

private:
    template <auto... Members>
    static void addCompositeUnique(CompositeUniqueness& constraints, CompositeUnique<Members...>) {
        static_assert((std::same_as<Fields, typename detail::DaoMember<decltype(Members)>::Record> && ...),
                      "CompositeUnique members must belong to the DAO fields type");
        constraints.push_back({std::string(glz::get_name<Members>())...});
    }

    template <typename... Constraints>
    static void addCompositeUnique(CompositeUniqueness& result, const std::tuple<Constraints...>& constraints) {
        std::apply([&](const auto&... constraint) { (addCompositeUnique(result, constraint), ...); }, constraints);
    }

    static CompositeUniqueness compositeUniqueness() {
        CompositeUniqueness result;
        if constexpr (requires { Fields::constraints; }) {
            addCompositeUnique(result, Fields::constraints);
        }
        return result;
    }

    const drogon::orm::DbClientPtr& transactionClient(const DaoTransaction& tx) const {
        if (std::addressof(tx.backend()) != database_.get()) {
            throw std::invalid_argument("DAO and transaction must use the same database backend");
        }
        return tx.client();
    }

    drogon::Task<std::optional<Record>> readById(TransactionScope tx, Id id, bool include = false) const {
        const auto rows = co_await execute(tx, select_ + " WHERE " + key_ + " = " + parameter(1) +
            (include ? "" : " AND deleted_at IS NULL"), ownArgument(std::move(id)));
        if (rows.empty()) co_return std::nullopt;
        co_return mapRow(rows[0]);
    }

    template <auto... Members, typename... TargetDaos>
    drogon::Task<std::optional<Record>> readWithForeign(
        TransactionScope tx, Id id, bool include, detail::ForeignSelection<Members, TargetDaos>... selections) const {
        if (!(selections.dao.get().usesBackend(database_) && ...)) {
            throw std::invalid_argument("foreign DAOs must use the same database backend");
        }
        auto record = co_await readById(tx, std::move(id), include);
        if (record) {
            if (tx) {
                ((co_await selections.dao.get().loadForeign(tx->get(), detail::foreignField((*record).*Members))), ...);
            } else {
                ((co_await selections.dao.get().loadForeign(detail::foreignField((*record).*Members))), ...);
            }
        }
        co_return record;
    }

    drogon::Task<Id> createRecord(TransactionScope tx, Record record) const {
        checkId<true>(record);
        const auto result = co_await writeRecord<true>(tx, insert_, record);
        co_return detail::readScalar<Id>(result[0][0]);
    }

    drogon::Task<bool> updateRecord(TransactionScope tx, Record record) const {
        checkId<false>(record);
        const auto result = co_await writeRecord<false>(tx, update_, record);
        co_return result.affectedRows() != 0;
    }

    drogon::Task<> writeBatch(TransactionScope tx, BatchCallback work) const {
        if (tx) return database_->savepoint(tx->get(), std::move(work));
        return database_->transaction(std::move(work));
    }

    drogon::Task<std::vector<Id>> createBatch(TransactionScope tx, std::vector<Record> records) const {
        std::vector<Id> ids;
        if (records.empty()) co_return ids;
        ids.reserve(records.size());
        co_await writeBatch(tx, [&](const DaoTransaction& batchTx) -> drogon::Task<> {
            for (auto& record : records) ids.push_back(co_await create(batchTx, std::move(record)));
        });
        co_return ids;
    }

    struct UnmatchedBulkUpdate {};

    drogon::Task<bool> updateBatch(TransactionScope tx, std::vector<Record> records) const {
        if (records.empty()) co_return true;
        try {
            co_await writeBatch(tx, [&](const DaoTransaction& batchTx) -> drogon::Task<> {
                for (auto& record : records) {
                    if (!co_await update(batchTx, std::move(record))) throw UnmatchedBulkUpdate{};
                }
            });
        } catch (const UnmatchedBulkUpdate&) {
            co_return false;
        }
        co_return true;
    }

    drogon::Task<bool> deleteRecord(TransactionScope tx, Id id) const {
        const auto result = co_await execute(tx, "DELETE FROM " + table_ + " WHERE " + key_ + " = " + parameter(1),
                                            ownArgument(std::move(id)));
        co_return result.affectedRows() != 0;
    }

    drogon::Task<bool> setDeleted(TransactionScope tx, Id id, bool deleted) const {
        const std::string value(deleted ? database_->currentTimestampSql() : "NULL");
        const auto result = co_await execute(tx, "UPDATE " + table_ + " SET deleted_at = " + value +
            " WHERE " + key_ + " = " + parameter(1) +
            (deleted ? " AND deleted_at IS NULL" : " AND deleted_at IS NOT NULL"),
            ownArgument(std::move(id)));
        co_return result.affectedRows() != 0;
    }

    template <typename... Predicates>
    drogon::Task<std::int64_t> countRecords(TransactionScope tx, bool include, Predicates... predicates) const {
        std::string sql = "SELECT COUNT(*) FROM " + table_;
        std::vector<detail::QueryValue> values;
        appendWhere(sql, values, include, predicates...);
        const auto rows = co_await executeBound(tx, std::move(sql), std::move(values));
        co_return rows[0][0].template as<std::int64_t>();
    }

    static constexpr std::array<std::string_view, 2> timestampNames_{"created_at", "updated_at"};
    static constexpr auto names_ = glz::reflect<Fields>::keys;
    static constexpr auto indices_ = std::make_index_sequence<names_.size()>{};

    Record mapRow(const drogon::orm::Row& row, std::vector<std::string> omitted = {}) const {
        if constexpr (!Options.timestamps) {
            for (const auto name : timestampNames_) omitted.emplace_back(name);
        }
        auto fields = rowToStruct<Fields>(row, omitted);
        Record result(std::move(fields), std::move(omitted));
        if (result.isColumnLoaded("deleted_at")) {
            detail::readRowField(result.deleted_at, row.at("deleted_at"), "deleted_at");
        }
        if constexpr (Record::generatedId) result.id = row.at("id").template as<Id>();
        if constexpr (Options.timestamps) {
            if (result.isColumnLoaded("created_at")) {
                detail::readRowField(result.created_at, row.at("created_at"), "created_at");
            }
            if (result.isColumnLoaded("updated_at")) {
                detail::readRowField(result.updated_at, row.at("updated_at"), "updated_at");
            }
        }
        return result;
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
        static_assert(name != "deleted_at" && !timestampName(name) && (!Record::generatedId || name != "id"),
                      "reserved DAO fields are inherited");
        static_assert(!Traits<I>::foreignKey() || !Traits<I>::primary, "foreign fields cannot be primary keys");
        auto field = DatabaseColumn{std::string(name), detail::columnType<typename Value<I>::Type>(),
                           Value<I>::nullable, Traits<I>::primary, Traits<I>::unique, Traits<I>::indexed,
                           Traits<I>::defaultMetadata()};
        field.foreignKey = Traits<I>::foreignKey();
        using Scalar = typename Value<I>::Type;
        if constexpr (std::is_scoped_enum_v<Scalar>) {
            constexpr auto labels = magic_enum::enum_names<Scalar>();
            static_assert(!labels.empty(), "DAO enum has no reflectable values");
            field.enumLabels.assign(labels.begin(), labels.end());
            std::ranges::sort(field.enumLabels);
        }
        if (field.foreignKey && field.defaultValue) {
            throw std::invalid_argument("foreign fields cannot declare a default");
        }
        schema_.push_back(std::move(field));
    }

    template <std::size_t... I>
    void buildSchema(std::index_sequence<I...>) {
        (addField<I>(), ...);
        if constexpr (Record::generatedId) {
            schema_.push_back({"id", ColumnType::integer, false, true, false, false, {}, true});
        }
        schema_.push_back({"deleted_at", ColumnType::datetime, true, false, false, false, {}});
        if constexpr (Options.timestamps) {
            for (const auto name : timestampNames_) {
                DatabaseColumn field{std::string(name), ColumnType::datetime, true, false, false, true, {}, false, true};
                field.indexDescending = true;
                schema_.push_back(std::move(field));
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

    struct FindQuery {
        EqualityFilters equality;
        std::vector<std::string_view> selected;
        std::vector<std::pair<std::string_view, SortDirection>> ordering;
        bool all = false;
        bool includeDeleted = false;
    };

    static void appendColumn(std::string& list, std::string_view column) {
        if (!list.empty()) list += ", ";
        list += column;
    }

    static void requireEnabledColumn(std::string_view name) {
        if (!Options.timestamps && timestampName(name)) {
            throw std::invalid_argument("timestamp queries require timestamps to be enabled");
        }
    }

    static auto collect(FindQuery& query, EqualityFilters& filters) {
        auto& entries = query.equality.entries_;
        entries.insert(entries.end(), std::make_move_iterator(filters.entries_.begin()),
                       std::make_move_iterator(filters.entries_.end()));
        return std::tuple{};
    }

    static auto collect(FindQuery& query, detail::IncludeDeleted) {
        query.includeDeleted = true;
        return std::tuple{};
    }

    static auto collect(FindQuery& query, detail::SelectAll) {
        query.all = true;
        return std::tuple{};
    }

    template <auto... Members>
    static auto collect(FindQuery& query, detail::SelectFields<Members...>) {
        ((requireEnabledColumn(glz::get_name<Members>()),
          query.selected.push_back(glz::get_name<Members>())), ...);
        return std::tuple{};
    }

    template <auto Member>
    static auto collect(FindQuery& query, detail::OrderBy<Member> order) {
        constexpr auto name = glz::get_name<Member>();
        requireEnabledColumn(name);
        query.ordering.emplace_back(name, order.direction);
        return std::tuple{};
    }

    template <auto Member>
    static auto collect(FindQuery&, detail::DaoPredicate<Member>& predicate) {
        requireEnabledColumn(predicate.name);
        return std::tie(predicate);
    }

    template <auto First, auto Second>
    static auto collect(FindQuery&, detail::DaoTupleLess<First, Second>& predicate) {
        return std::tie(predicate);
    }

    template <auto Member, typename Dao>
    static auto collect(FindQuery& query, detail::ForeignSelection<Member, Dao>) {
        query.selected.push_back(glz::get_name<Member>());
        return std::tuple{};
    }

    static auto foreignSelection(const auto&) { return std::tuple{}; }

    template <auto Member, typename Dao>
    static auto foreignSelection(detail::ForeignSelection<Member, Dao> selection) {
        return std::tuple{selection};
    }

    drogon::Task<std::map<Id, Record>> readForeignBatch(
        TransactionScope tx, const std::set<Id>& keys) const {
        constexpr std::size_t batchSize = 500;
        const std::string keyName(Record::keyColumn);
        std::map<Id, Record> records;
        auto next = keys.begin();
        while (next != keys.end()) {
            std::string placeholders;
            std::vector<detail::QueryValue> values;
            while (next != keys.end() && values.size() < batchSize) {
                values.push_back(queryValue(*next++));
                appendColumn(placeholders, parameter(values.size()));
            }
            auto rows = co_await executeBound(tx, select_ + " WHERE " + key_ +
                " IN (" + placeholders + ") AND deleted_at IS NULL", std::move(values));
            for (const auto& row : rows) {
                auto key = detail::readScalar<Id>(row.at(keyName));
                records.emplace(std::move(key), mapRow(row));
            }
        }
        co_return records;
    }

    template <auto Member, typename Dao>
    static drogon::Task<> loadForeignPage(TransactionScope tx, std::vector<Record>& records,
                                           detail::ForeignSelection<Member, Dao> selection) {
        using TargetId = typename Dao::RecordType::Id;
        std::set<TargetId> keys;
        for (auto& record : records) {
            const std::optional<TargetId> key(detail::foreignField(record.*Member).key());
            if (key) keys.insert(*key);
        }
        if (keys.empty()) co_return;
        const auto targets = co_await selection.dao.get().readForeignBatch(tx, keys);
        for (auto& record : records) {
            auto& field = detail::foreignField(record.*Member);
            const std::optional<TargetId> key(field.key());
            if (!key) continue;
            const auto target = targets.find(*key);
            if (target != targets.end()) field.setRecord(target->second);
        }
    }

    template <typename... Modifiers>
    drogon::Task<std::vector<Record>> findPage(
        TransactionScope tx, std::int64_t limit, std::int64_t offset, Modifiers... modifiers) const {
        if (limit <= 0 || offset < 0) throw std::invalid_argument("invalid pagination");
        const auto foreign = std::tuple_cat(foreignSelection(modifiers)...);
        const auto sameBackend = std::apply([&](const auto&... selections) {
            return (selections.dao.get().usesBackend(database_) && ...);
        }, foreign);
        if (!sameBackend) throw std::invalid_argument("foreign DAOs must use the same database backend");

        FindQuery query;
        // Collect left to right so explicit sort fields retain the caller's order.
        auto parts = std::tuple<decltype(collect(query, modifiers))...>{collect(query, modifiers)...};
        const auto predicates = std::apply([](auto&&... part) {
            return std::tuple_cat(std::move(part)...);
        }, parts);

        std::string sql = select_;
        std::vector<std::string> omitted;
        if (!query.all) {
            auto projection = requiredProjection_;
            for (const auto& name : optionalColumns_) {
                if (std::ranges::find(query.selected, name) == query.selected.end()) omitted.push_back(name);
                else appendColumn(projection, detail::quoteIdentifier(name));
            }
            sql = "SELECT " + projection + " FROM " + table_;
        }
        if (query.ordering.empty() && Options.timestamps) {
            query.ordering.emplace_back("updated_at", SortDirection::desc);
        }
        if (std::ranges::none_of(query.ordering, [](const auto& item) { return item.first == Record::keyColumn; })) {
            query.ordering.emplace_back(Record::keyColumn, SortDirection::asc);
        }
        std::string ordering;
        for (const auto& [name, direction] : query.ordering) {
            appendColumn(ordering, detail::quoteIdentifier(name) +
                (direction == SortDirection::desc ? " DESC NULLS LAST" : " ASC NULLS LAST"));
        }
        std::vector<detail::QueryValue> values;
        std::apply([&](const auto&... predicates) { appendWhere(sql, values, query.includeDeleted, predicates...); }, predicates);
        bool hasWhere = !query.includeDeleted || std::tuple_size_v<decltype(predicates)> != 0;
        for (auto& [name, value] : query.equality.entries_) {
            requireEnabledColumn(name);
            sql += hasWhere ? " AND " : " WHERE ";
            hasWhere = true;
            values.push_back(std::move(value));
            sql += database_->nullSafeEqual(detail::quoteIdentifier(name), values.size());
        }
        sql += " ORDER BY " + ordering + " LIMIT " + parameter(values.size() + 1) +
               " OFFSET " + parameter(values.size() + 2);
        values.emplace_back(limit);
        values.emplace_back(offset);
        const auto rows = co_await executeBound(tx, std::move(sql), std::move(values));

        std::vector<Record> records;
        records.reserve(rows.size());
        for (const auto& row : rows) records.push_back(mapRow(row, omitted));
        if constexpr (std::tuple_size_v<decltype(foreign)> > 0) {
            const auto load = [&](const auto&... selections) -> drogon::Task<> {
                (co_await loadForeignPage(tx, records, selections), ...);
            };
            co_await std::apply(load, foreign);
        }
        co_return records;
    }

    template <typename T>
    static detail::QueryValue queryValue(const T& value) {
        auto owned = ownArgument(value);
        using Owned = decltype(owned);
        if constexpr (detail::Nullable<Owned>::nullable) {
            if (!owned) return nullptr;
            return queryValue(*owned);
        } else if constexpr (std::signed_integral<Owned>) {
            return static_cast<std::int64_t>(owned);
        } else {
            return owned;
        }
    }

    template <auto Member>
    std::string predicateSql(const detail::DaoPredicate<Member>& predicate,
                             std::vector<detail::QueryValue>& values) const {
        using detail::PredicateOp;
        requireEnabledColumn(predicate.name);
        const auto column = detail::quoteIdentifier(predicate.name);
        std::string expression;
        if (predicate.operation == PredicateOp::in) {
            std::string list;
            bool includesNull = false;
            for (const typename detail::DaoPredicate<Member>::Value value : predicate.values) {
                auto argument = queryValue(value);
                if (std::holds_alternative<std::nullptr_t>(argument)) {
                    includesNull = true;
                } else {
                    values.push_back(std::move(argument));
                    appendColumn(list, parameter(values.size()));
                }
            }
            expression = list.empty() ? "FALSE" : column + " IN (" + list + ")";
            if (includesNull) expression += " OR " + column + " IS NULL";
        } else {
            values.push_back(queryValue(static_cast<typename detail::DaoPredicate<Member>::Value>(
                predicate.values.front())));
            const auto index = values.size();
            switch (predicate.operation) {
                case PredicateOp::eq: expression = database_->nullSafeEqual(column, index); break;
                case PredicateOp::gt: expression = column + " > " + parameter(index); break;
                case PredicateOp::gte: expression = column + " >= " + parameter(index); break;
                case PredicateOp::lt: expression = column + " < " + parameter(index); break;
                case PredicateOp::lte: expression = column + " <= " + parameter(index); break;
                case PredicateOp::contains: expression = database_->containsText(column, index); break;
                case PredicateOp::in: break;
            }
        }
        if (predicate.negated) return "NOT COALESCE((" + expression + "), FALSE)";
        return "(" + expression + ")";
    }

    template <auto First, auto Second>
    std::string predicateSql(const detail::DaoTupleLess<First, Second>& predicate,
                             std::vector<detail::QueryValue>& values) const {
        constexpr auto firstColumn = glz::get_name<First>();
        constexpr auto secondColumn = glz::get_name<Second>();
        requireEnabledColumn(firstColumn);
        requireEnabledColumn(secondColumn);
        const auto firstParameter = parameter(values.size() + 1);
        const auto secondParameter = parameter(values.size() + 2);
        values.push_back(queryValue(predicate.first));
        values.push_back(queryValue(predicate.second));
        return "(" + detail::quoteIdentifier(firstColumn) + ", " + detail::quoteIdentifier(secondColumn) +
               ") < (" + firstParameter + ", " + secondParameter + ")";
    }

    template <typename... Predicates>
    void appendWhere(std::string& sql, std::vector<detail::QueryValue>& values, bool include,
                     const Predicates&... predicates) const {
        if (!include) sql += " WHERE deleted_at IS NULL";
        if constexpr (sizeof...(Predicates) > 0) {
            auto separator = include ? " WHERE " : " AND ";
            auto append = [&](const auto& predicate) {
                sql += separator + predicateSql(predicate, values);
                separator = " AND ";
            };
            (append(predicates), ...);
        }
    }

    drogon::Task<drogon::orm::Result> executeBound(
        TransactionScope tx, std::string sql, std::vector<detail::QueryValue> values) const {
        if (tx) {
            co_return co_await detail::executeBoundOn(transactionClient(tx->get()), std::move(sql), std::move(values));
        }
        if (const auto gate = database_->queryGate()) {
            const auto lock = co_await gate->get().scoped_lock();
            co_return co_await detail::executeBoundOn(database_->client(), std::move(sql), std::move(values));
        }
        co_return co_await detail::executeBoundOn(database_->client(), std::move(sql), std::move(values));
    }

    template <std::size_t I, typename Field>
    static decltype(auto) updateValue(const Record& record, Field& field) {
        if constexpr (Value<I>::nullable) {
            using Argument = typename Traits<I>::Value;
            return record.isColumnLoaded(names_[I]) ? Argument(Traits<I>::unwrap(field)) : Argument{};
        } else {
            return Traits<I>::unwrap(field);
        }
    }

    template <std::size_t I>
    static auto updateFlag(const Record& record) {
        if constexpr (Value<I>::nullable) return std::tuple{record.isColumnLoaded(names_[I])};
        else return std::tuple{};
    }

    template <bool Insert>
    drogon::Task<drogon::orm::Result> writeRecord(TransactionScope tx, const std::string& sql, Record& record) const {
        auto fields = glz::to_tie(static_cast<Fields&>(record));
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            if constexpr (Insert) {
                return execute(tx, sql, ownArgument(Traits<I>::unwrap(glz::get<I>(fields)))...);
            } else {
                return std::apply([&](auto... flags) {
                    if constexpr (Record::generatedId) {
                        return execute(tx, sql, ownArgument(updateValue<I>(record, glz::get<I>(fields)))..., *record.id, flags...);
                    } else {
                        return execute(tx, sql, ownArgument(updateValue<I>(record, glz::get<I>(fields)))..., flags...);
                    }
                }, std::tuple_cat(updateFlag<I>(record)...));
            }
        }(indices_);
    }

    template <typename T>
    static auto ownArgument(T&& value) {
        using Argument = std::remove_cvref_t<T>;
        using Scalar = typename detail::Nullable<Argument>::Type;
        if constexpr (std::is_scoped_enum_v<Argument>) {
            return std::string(detail::enumLabel(value));
        } else if constexpr (std::is_scoped_enum_v<Scalar>) {
            return value ? std::optional{std::string(detail::enumLabel(*value))} : std::nullopt;
        } else if constexpr (std::same_as<Argument, DaoDateTime>) {
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
    drogon::Task<drogon::orm::Result> execute(TransactionScope tx, std::string sql, Args... args) const {
        if (tx) {
            co_return co_await detail::executeOn(transactionClient(tx->get()), std::move(sql), std::move(args)...);
        }
        if (const auto gate = database_->queryGate()) {
            const auto lock = co_await gate->get().scoped_lock();
            co_return co_await detail::executeOn(database_->client(), std::move(sql), std::move(args)...);
        }
        co_return co_await detail::executeOn(database_->client(), std::move(sql), std::move(args)...);
    }

    DatabaseBackendPtr database_;
    std::string tableName_, table_, key_, select_, insert_, update_;
    std::vector<DatabaseColumn> schema_;
    std::vector<std::string> optionalColumns_;
    std::string requiredProjection_;
};

}
