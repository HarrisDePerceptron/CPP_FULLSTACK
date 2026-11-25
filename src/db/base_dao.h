#pragma once

#include "database_backend.h"
#include "dao_record.h"
#include "dao_predicate.h"
#include "row_to_struct.h"

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

template <auto... members>
struct SelectFields {};

struct SelectAll {};
struct IncludeDeleted {};

template <auto member>
struct OrderBy {
    SortDirection direction;
};

template <auto first, auto... rest>
consteval bool valid_composite_members() {
    using Owner = typename DaoMember<decltype(first)>::Record;
    if constexpr (!(std::same_as<Owner, typename DaoMember<decltype(rest)>::Record> && ...)) {
        return false;
    } else {
        constexpr std::array names{glz::get_name<first>(), glz::get_name<rest>()...};
        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = i + 1; j < names.size(); ++j) {
                if (names[i] == names[j]) return false;
            }
        }
        return true;
    }
}

}

inline auto include_deleted() { return detail::IncludeDeleted{}; }

template <auto... members>
    requires (sizeof...(members) >= 2) &&
             (std::is_member_object_pointer_v<decltype(members)> && ...) &&
             (detail::valid_composite_members<members...>())
struct CompositeUnique {};

struct DaoOptions { bool timestamps = true; };

template <typename Derived, typename Record, DaoOptions options = {}>
class BaseDao {
    template <typename, typename, DaoOptions>
    friend class BaseDao;

    using Fields = typename Record::FieldRecord;
    using Id = typename Record::Id;
    using TransactionScope = std::optional<std::reference_wrapper<const DaoTransaction>>;

    template <auto member>
    static constexpr bool record_member =
        std::same_as<detail::DaoDeletion, typename detail::DaoPredicate<member>::Record> ||
        std::same_as<Fields, typename detail::DaoPredicate<member>::Record> ||
        std::same_as<detail::DaoTimestamps<Fields>, typename detail::DaoPredicate<member>::Record> ||
        (Record::generated_id &&
         std::same_as<detail::DaoGeneratedKey<Fields, true>, typename detail::DaoPredicate<member>::Record>);

    template <auto member>
    using FieldValue = typename detail::DaoMember<decltype(member)>::Type;

    template <auto member>
    using ModelField = std::remove_cvref_t<decltype(std::declval<Fields&>().*member)>;

    template <auto member>
    static constexpr bool model_member = std::same_as<Fields, typename detail::DaoMember<decltype(member)>::Record>;

    template <auto member>
    static constexpr bool writable_member = [] {
        if constexpr (model_member<member>) return !detail::FieldTraits<ModelField<member>>::primary;
        return false;
    }();

    template <auto member>
    static constexpr bool conflict_member = [] {
        if constexpr (model_member<member>) {
            using Traits = detail::FieldTraits<ModelField<member>>;
            return Traits::primary || Traits::unique;
        }
        return false;
    }();

    template <typename Builder>
    class NamedFilters {
        Builder&& builder() && { return static_cast<Builder&&>(*this); }

    public:
        template <auto member, typename Value>
            requires record_member<member> &&
                     requires(Value&& value) { APP_NAMESPACE::where<member>(APP_NAMESPACE::gt(std::forward<Value>(value))); }
        auto where_gt(Value&& value) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::gt(std::forward<Value>(value)));
        }

        template <auto member, typename Value>
            requires record_member<member> &&
                     requires(Value&& value) { APP_NAMESPACE::where<member>(APP_NAMESPACE::gte(std::forward<Value>(value))); }
        auto where_gte(Value&& value) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::gte(std::forward<Value>(value)));
        }

        template <auto member, typename Value>
            requires record_member<member> &&
                     requires(Value&& value) { APP_NAMESPACE::where<member>(APP_NAMESPACE::lt(std::forward<Value>(value))); }
        auto where_lt(Value&& value) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::lt(std::forward<Value>(value)));
        }

        template <auto member, typename Value>
            requires record_member<member> &&
                     requires(Value&& value) { APP_NAMESPACE::where<member>(APP_NAMESPACE::lte(std::forward<Value>(value))); }
        auto where_lte(Value&& value) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::lte(std::forward<Value>(value)));
        }

        template <auto member, typename Value>
            requires record_member<member> &&
                     requires(Value&& value) { APP_NAMESPACE::where<member>(APP_NAMESPACE::contains(std::forward<Value>(value))); }
        auto where_contains(Value&& value) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::contains(std::forward<Value>(value)));
        }

        template <auto member, std::ranges::input_range Range>
            requires record_member<member> &&
                     requires(Range&& values) { APP_NAMESPACE::where<member>(APP_NAMESPACE::in(std::forward<Range>(values))); }
        auto where_in(Range&& values) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::in(std::forward<Range>(values)));
        }

        template <auto member>
            requires record_member<member>
        auto where_in(std::initializer_list<FieldValue<member>> values) && {
            return std::move(*this).builder().template where<member>(APP_NAMESPACE::in(values));
        }
    };

    static constexpr bool valid_filter(...) { return false; }

    template <auto member>
    static constexpr bool valid_filter(std::type_identity<detail::DaoPredicate<member>>) {
        return record_member<member>;
    }

    template <auto first, auto second>
    static constexpr bool valid_filter(std::type_identity<detail::DaoTupleLess<first, second>>) {
        return record_member<first> && record_member<second>;
    }

public:
    using RecordType = Record;

    class EqualityFilters {
    public:
        template <auto member, typename Value>
            requires std::is_member_object_pointer_v<decltype(member)> &&
                     (record_member<member>) && requires(Value value) { where<member>(value); }
        void equal(Value value) {
            const auto predicate = where<member>(std::move(value));
            using FieldValue = typename detail::DaoPredicate<member>::Value;
            entries_.emplace_back(predicate.name, query_value(static_cast<FieldValue>(predicate.values.front())));
        }

    private:
        friend BaseDao;
        std::vector<std::pair<std::string_view, detail::QueryValue>> entries_;
    };

    template <typename... Modifiers>
    class FindBuilder : public NamedFilters<FindBuilder<Modifiers...>> {
    public:
        FindBuilder limit(std::int64_t value) && {
            limit_ = value;
            return std::move(*this);
        }

        FindBuilder offset(std::int64_t value) && {
            offset_ = value;
            return std::move(*this);
        }

        template <auto member, typename Value>
            requires requires(Value&& value) { APP_NAMESPACE::where<member>(std::forward<Value>(value)); } &&
                     record_member<member>
        auto where(Value&& value) && {
            return std::move(*this).where(APP_NAMESPACE::where<member>(std::forward<Value>(value)));
        }

        template <typename Predicate>
            requires (valid_filter(std::type_identity<Predicate>{}))
        auto where(Predicate predicate) && {
            return std::move(*this).append(std::move(predicate));
        }

        template <auto... members>
            requires (sizeof...(members) > 0) &&
                     (std::is_member_object_pointer_v<decltype(members)> && ...) &&
                     (record_member<members> && ...)
        auto select() && {
            return std::move(*this).append(detail::SelectFields<members...>{});
        }

        auto select_all() && { return std::move(*this).append(detail::SelectAll{}); }

        template <auto member>
            requires std::is_member_object_pointer_v<decltype(member)> && record_member<member>
        auto order_by(SortDirection direction = SortDirection::asc) && {
            return std::move(*this).append(detail::OrderBy<member>{direction});
        }

        auto include_deleted() && { return std::move(*this).append(detail::IncludeDeleted{}); }

        template <auto member, typename Dao>
            requires detail::ForeignSelectionFor<member, Record, std::remove_cv_t<Dao>>
        auto with_foreign(Dao& dao) && {
            return std::move(*this).append(APP_NAMESPACE::with_foreign<member>(dao));
        }

        auto matching(EqualityFilters filters) && {
            return std::move(*this).append(std::move(filters));
        }

        drogon::Task<std::vector<Record>> collect() && {
            return std::apply([&](auto&&... modifiers) {
                return dao_.find_page(tx_, limit_, offset_, std::move(modifiers)...);
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

    drogon::Task<> transaction(TransactionCallback work) const & {
        return database_->transaction(std::move(work));
    }

    drogon::Task<> transaction(TransactionCallback work) const && = delete;

    template <bool nullable>
    drogon::Task<bool> load_foreign(Foreign<Record, nullable>& field) const & {
        return field.load(*this);
    }

    template <bool nullable>
    drogon::Task<bool> load_foreign(Foreign<Record, nullable>& field) const && = delete;

    template <bool nullable>
    drogon::Task<bool> load_foreign(const DaoTransaction& tx, Foreign<Record, nullable>& field) const & {
        transaction_client(tx);
        return field.load(*this, tx);
    }

    template <bool nullable>
    drogon::Task<bool> load_foreign(const DaoTransaction& tx, Foreign<Record, nullable>& field) const && = delete;

    template <typename... Selections>
    class FindByIdBuilder {
    public:
        FindByIdBuilder include_deleted() && {
            include_ = true;
            return std::move(*this);
        }

        template <auto member, typename Dao>
            requires detail::ForeignSelectionFor<member, Record, std::remove_cv_t<Dao>>
        auto with_foreign(Dao& dao) && {
            auto selection = APP_NAMESPACE::with_foreign<member>(dao);
            return FindByIdBuilder<Selections..., decltype(selection)>(dao_, tx_, std::move(id_),
                include_, std::tuple_cat(std::move(selections_), std::tuple{selection}));
        }

        drogon::Task<std::optional<Record>> collect() && {
            return std::apply([&](auto... selections) {
                return dao_.read_with_foreign(tx_, std::move(id_), include_, selections...);
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

    FindByIdBuilder<> find_by_id(Id id) const & {
        return FindByIdBuilder<>(*this, {}, std::move(id), false, {});
    }

    FindByIdBuilder<> find_by_id(const DaoTransaction& tx, Id id) const & {
        return FindByIdBuilder<>(*this, std::cref(tx), std::move(id), false, {});
    }

    FindByIdBuilder<> find_by_id(Id) const && = delete;
    FindByIdBuilder<> find_by_id(const DaoTransaction&, Id) const && = delete;

    bool uses_backend(const DatabaseBackendPtr& database) const {
        return database_ == database;
    }

    drogon::Task<Id> create(Record record) const & {
        return create_record({}, std::move(record));
    }

    drogon::Task<Id> create(Record record) const && = delete;

    drogon::Task<Id> create(const DaoTransaction& tx, Record record) const & {
        return create_record(std::cref(tx), std::move(record));
    }

    drogon::Task<Id> create(const DaoTransaction& tx, Record record) const && = delete;

    template <auto conflict_key>
        requires (conflict_member<conflict_key>)
    drogon::Task<bool> insert_if_absent(Record record) const & {
        return insert_absent<conflict_key>({}, std::move(record));
    }

    template <auto conflict_key>
        requires (conflict_member<conflict_key>)
    drogon::Task<bool> insert_if_absent(const DaoTransaction& tx, Record record) const & {
        return insert_absent<conflict_key>(std::cref(tx), std::move(record));
    }

    template <auto conflict_key>
    drogon::Task<bool> insert_if_absent(Record) const && = delete;
    template <auto conflict_key>
    drogon::Task<bool> insert_if_absent(const DaoTransaction&, Record) const && = delete;

    template <auto conflict_key, auto... members>
        requires (sizeof...(members) > 0) && (conflict_member<conflict_key>) &&
                 (writable_member<members> && ...) && (detail::valid_composite_members<conflict_key, members...>())
    drogon::Task<Id> upsert(Record record) const & {
        return upsert_record<conflict_key, members...>({}, std::move(record));
    }

    template <auto conflict_key, auto... members>
        requires (sizeof...(members) > 0) && (conflict_member<conflict_key>) &&
                 (writable_member<members> && ...) && (detail::valid_composite_members<conflict_key, members...>())
    drogon::Task<Id> upsert(const DaoTransaction& tx, Record record) const & {
        return upsert_record<conflict_key, members...>(std::cref(tx), std::move(record));
    }

    template <auto conflict_key, auto... members>
    drogon::Task<Id> upsert(Record) const && = delete;
    template <auto conflict_key, auto... members>
    drogon::Task<Id> upsert(const DaoTransaction&, Record) const && = delete;

    class UpdateByIdBuilder {
    public:
        template <auto member, typename Value>
            requires writable_member<member> && std::constructible_from<FieldValue<member>, Value>
        UpdateByIdBuilder set(Value&& value) && {
            constexpr auto column = glz::get_name<member>();
            auto argument = query_value(FieldValue<member>(std::forward<Value>(value)));
            auto existing = std::ranges::find(assignments_, column, &Assignment::column);
            if (existing == assignments_.end()) assignments_.push_back({column, std::move(argument)});
            else existing->value = std::move(argument);
            return std::move(*this);
        }

        drogon::Task<bool> execute() && {
            return dao_.patch_record(tx_, std::move(id_), std::move(assignments_));
        }

    private:
        friend BaseDao;
        struct Assignment {
            std::string_view column;
            detail::QueryValue value;
        };

        UpdateByIdBuilder(const BaseDao& dao, TransactionScope tx, Id id)
            : dao_(dao), tx_(tx), id_(std::move(id)) {}

        const BaseDao& dao_;
        TransactionScope tx_;
        Id id_;
        std::vector<Assignment> assignments_;
    };

    UpdateByIdBuilder update_by_id(Id id) const & {
        return UpdateByIdBuilder(*this, {}, std::move(id));
    }

    UpdateByIdBuilder update_by_id(const DaoTransaction& tx, Id id) const & {
        return UpdateByIdBuilder(*this, std::cref(tx), std::move(id));
    }

    UpdateByIdBuilder update_by_id(Id) const && = delete;
    UpdateByIdBuilder update_by_id(const DaoTransaction&, Id) const && = delete;

    drogon::Task<bool> update(Record record) const & {
        return update_record({}, std::move(record));
    }

    drogon::Task<bool> update(Record record) const && = delete;

    drogon::Task<bool> update(const DaoTransaction& tx, Record record) const & {
        return update_record(std::cref(tx), std::move(record));
    }

    drogon::Task<bool> update(const DaoTransaction& tx, Record record) const && = delete;

    drogon::Task<std::vector<Id>> create_many(std::vector<Record> records) const & {
        return create_batch({}, std::move(records));
    }

    drogon::Task<std::vector<Id>> create_many(std::vector<Record> records) const && = delete;

    drogon::Task<std::vector<Id>> create_many(const DaoTransaction& tx, std::vector<Record> records) const & {
        return create_batch(std::cref(tx), std::move(records));
    }

    drogon::Task<std::vector<Id>> create_many(const DaoTransaction& tx, std::vector<Record> records) const && = delete;

    drogon::Task<bool> update_many(std::vector<Record> records) const & {
        return update_batch({}, std::move(records));
    }

    drogon::Task<bool> update_many(std::vector<Record> records) const && = delete;

    drogon::Task<bool> update_many(const DaoTransaction& tx, std::vector<Record> records) const & {
        return update_batch(std::cref(tx), std::move(records));
    }

    drogon::Task<bool> update_many(const DaoTransaction& tx, std::vector<Record> records) const && = delete;

    drogon::Task<bool> delete_by_id(Id id) const & {
        return delete_record({}, std::move(id));
    }

    drogon::Task<bool> delete_by_id(Id id) const && = delete;

    drogon::Task<bool> delete_by_id(const DaoTransaction& tx, Id id) const & {
        return delete_record(std::cref(tx), std::move(id));
    }

    drogon::Task<bool> delete_by_id(const DaoTransaction& tx, Id id) const && = delete;

    drogon::Task<bool> soft_delete_by_id(Id id) const & {
        return set_deleted({}, std::move(id), true);
    }

    drogon::Task<bool> soft_delete_by_id(Id id) const && = delete;

    drogon::Task<bool> soft_delete_by_id(const DaoTransaction& tx, Id id) const & {
        return set_deleted(std::cref(tx), std::move(id), true);
    }

    drogon::Task<bool> soft_delete_by_id(const DaoTransaction& tx, Id id) const && = delete;

    drogon::Task<bool> restore_by_id(Id id) const & {
        return set_deleted({}, std::move(id), false);
    }

    drogon::Task<bool> restore_by_id(Id id) const && = delete;

    drogon::Task<bool> restore_by_id(const DaoTransaction& tx, Id id) const & {
        return set_deleted(std::cref(tx), std::move(id), false);
    }

    drogon::Task<bool> restore_by_id(const DaoTransaction& tx, Id id) const && = delete;

    drogon::Task<std::int64_t> delete_many(std::vector<Id> ids) const & {
        return delete_batch({}, std::move(ids), false);
    }

    drogon::Task<std::int64_t> delete_many(std::vector<Id> ids) const && = delete;

    drogon::Task<std::int64_t> delete_many(const DaoTransaction& tx, std::vector<Id> ids) const & {
        return delete_batch(std::cref(tx), std::move(ids), false);
    }

    drogon::Task<std::int64_t> delete_many(const DaoTransaction& tx, std::vector<Id> ids) const && = delete;

    drogon::Task<std::int64_t> soft_delete_many(std::vector<Id> ids) const & {
        return delete_batch({}, std::move(ids), true);
    }

    drogon::Task<std::int64_t> soft_delete_many(std::vector<Id> ids) const && = delete;

    drogon::Task<std::int64_t> soft_delete_many(const DaoTransaction& tx, std::vector<Id> ids) const & {
        return delete_batch(std::cref(tx), std::move(ids), true);
    }

    drogon::Task<std::int64_t> soft_delete_many(const DaoTransaction& tx, std::vector<Id> ids) const && = delete;

    template <typename... Predicates>
    class CountBuilder : public NamedFilters<CountBuilder<Predicates...>> {
    public:
        CountBuilder include_deleted() && {
            include_ = true;
            return std::move(*this);
        }

        template <auto member, typename Value>
            requires requires(Value&& value) { APP_NAMESPACE::where<member>(std::forward<Value>(value)); } &&
                     record_member<member>
        auto where(Value&& value) && {
            return std::move(*this).where(APP_NAMESPACE::where<member>(std::forward<Value>(value)));
        }

        template <typename Predicate>
            requires (valid_filter(std::type_identity<Predicate>{}))
        auto where(Predicate predicate) && {
            return CountBuilder<Predicates..., Predicate>(dao_, tx_, include_,
                std::tuple_cat(std::move(predicates_), std::tuple{std::move(predicate)}));
        }

        drogon::Task<std::int64_t> collect() && {
            return std::apply([&](auto&&... predicates) {
                return dao_.count_records(tx_, include_, std::move(predicates)...);
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
    drogon::Task<drogon::orm::Result> execute_sql(std::string sql, Args&&... args) const & {
        return execute({}, std::move(sql), own_argument(std::forward<Args>(args))...);
    }

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(std::string sql, Args&&... args) const && = delete;

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(const DaoTransaction& tx, std::string sql, Args&&... args) const & {
        return execute(std::cref(tx), std::move(sql), own_argument(std::forward<Args>(args))...);
    }

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(const DaoTransaction& tx, std::string sql, Args&&... args) const && = delete;

protected:
    static drogon::Task<Derived> finish_initialization(Derived dao) {
        auto& base = static_cast<BaseDao&>(dao);
        co_await base.database_->initialize_table(base.table_name_, std::move(base.schema_), composite_uniqueness());
        co_return std::move(dao);
    }

    BaseDao(const BaseDao&) = delete;
    BaseDao& operator=(const BaseDao&) = delete;
    BaseDao(BaseDao&&) noexcept = default;
    BaseDao& operator=(BaseDao&&) noexcept = default;

    explicit BaseDao(DatabaseBackendPtr database)
        : database_(std::move(database)), table_name_(Fields::table_name), table_(detail::quote_identifier(table_name_)) {
        if (!database_) throw std::invalid_argument("DAO requires a database backend");
        build_schema(indices_);
        std::string projection, columns, placeholders, assignments, key_parameter;
        std::size_t insert_index = 0, update_index = 0;
        std::size_t flag_index = names_.size() + Record::generated_id;
        for (const auto& field : schema_) {
            const auto column = detail::quote_identifier(field.name);
            if (!projection.empty()) projection += ", ";
            projection += column;
            if (field.nullable) optional_columns_.push_back(field.name);
            else append_column(required_projection_, column);
            if (field.timestamp || field.name == "deleted_at") continue;

            const auto marker = parameter(++update_index);
            if (field.primary) {
                key_ = column;
                key_parameter = marker;
            } else {
                if (!assignments.empty()) assignments += ", ";
                assignments += column + " = ";
                if (field.nullable) {
                    assignments += "CASE WHEN " + parameter(++flag_index) + " THEN " + marker +
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
                placeholders += parameter(++insert_index);
            }
        }
        if (assignments.empty()) assignments = key_ + " = " + key_;
        update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + key_parameter + " AND deleted_at IS NULL";
        select_ = "SELECT " + projection + " FROM " + table_;
        insert_ = "INSERT INTO " + table_;
        if (columns.empty()) insert_ += " DEFAULT VALUES";
        else insert_ += " (" + columns + ") VALUES (" + placeholders + ")";
    }

    std::string parameter(std::size_t index) const { return database_->parameter(index); }

private:
    template <auto... members>
    static void add_composite_unique(CompositeUniqueness& constraints, CompositeUnique<members...>) {
        static_assert((std::same_as<Fields, typename detail::DaoMember<decltype(members)>::Record> && ...),
                      "CompositeUnique members must belong to the DAO fields type");
        constraints.push_back({std::string(glz::get_name<members>())...});
    }

    template <typename... Constraints>
    static void add_composite_unique(CompositeUniqueness& result, const std::tuple<Constraints...>& constraints) {
        std::apply([&](const auto&... constraint) { (add_composite_unique(result, constraint), ...); }, constraints);
    }

    static CompositeUniqueness composite_uniqueness() {
        CompositeUniqueness result;
        if constexpr (requires { Fields::constraints; }) {
            add_composite_unique(result, Fields::constraints);
        }
        return result;
    }

    const drogon::orm::DbClientPtr& transaction_client(const DaoTransaction& tx) const {
        if (std::addressof(tx.backend()) != database_.get()) {
            throw std::invalid_argument("DAO and transaction must use the same database backend");
        }
        return tx.client();
    }

    drogon::Task<std::optional<Record>> read_by_id(TransactionScope tx, Id id, bool include = false) const {
        const auto rows = co_await execute(tx, select_ + " WHERE " + key_ + " = " + parameter(1) +
            (include ? "" : " AND deleted_at IS NULL"), own_argument(std::move(id)));
        if (rows.empty()) co_return std::nullopt;
        co_return map_row(rows[0]);
    }

    template <auto... members, typename... TargetDaos>
    drogon::Task<std::optional<Record>> read_with_foreign(
        TransactionScope tx, Id id, bool include, detail::ForeignSelection<members, TargetDaos>... selections) const {
        if (!(selections.dao.get().uses_backend(database_) && ...)) {
            throw std::invalid_argument("foreign DAOs must use the same database backend");
        }
        auto record = co_await read_by_id(tx, std::move(id), include);
        if (record) {
            if (tx) {
                ((co_await selections.dao.get().load_foreign(tx->get(), detail::foreign_field((*record).*members))), ...);
            } else {
                ((co_await selections.dao.get().load_foreign(detail::foreign_field((*record).*members))), ...);
            }
        }
        co_return record;
    }

    drogon::Task<Id> create_record(TransactionScope tx, Record record) const {
        check_id<true>(record);
        const auto result = co_await write_record<true>(tx, insert_ + " RETURNING " + key_, record);
        co_return detail::read_scalar<Id>(result[0][0]);
    }

    template <auto conflict_key>
    static std::string conflict_target() {
        return " ON CONFLICT (" + detail::quote_identifier(glz::get_name<conflict_key>()) + ")";
    }

    template <auto conflict_key>
    drogon::Task<bool> insert_absent(TransactionScope tx, Record record) const {
        check_id<true>(record);
        const auto result = co_await write_record<true>(tx, insert_ + conflict_target<conflict_key>() + " DO NOTHING", record);
        co_return result.affectedRows() != 0;
    }

    template <auto conflict_key, auto... members>
    drogon::Task<Id> upsert_record(TransactionScope tx, Record record) const {
        check_id<true>(record);
        std::string assignments;
        const auto add = [&](std::string_view name) {
            auto column = detail::quote_identifier(name);
            append_column(assignments, column + " = excluded." + column);
        };
        (add(glz::get_name<members>()), ...);
        const auto result = co_await write_record<true>(tx, insert_ + conflict_target<conflict_key>() +
            " DO UPDATE SET " + assignments + " RETURNING " + key_, record);
        co_return detail::read_scalar<Id>(result[0][0]);
    }

    drogon::Task<bool> patch_record(TransactionScope tx, Id id,
                                 std::vector<typename UpdateByIdBuilder::Assignment> fields) const {
        if (fields.empty()) throw std::invalid_argument("updateById requires at least one field");
        std::string assignments;
        std::vector<detail::QueryValue> values;
        values.reserve(fields.size() + 1);
        for (auto& field : fields) {
            values.push_back(std::move(field.value));
            append_column(assignments, detail::quote_identifier(field.column) + " = " + parameter(values.size()));
        }
        values.push_back(query_value(id));
        auto sql = "UPDATE " + table_ + " SET " + assignments +
            " WHERE " + key_ + " = " + parameter(values.size()) + " AND deleted_at IS NULL";
        const auto result = co_await execute_bound(tx, std::move(sql), std::move(values));
        co_return result.affectedRows() != 0;
    }

    drogon::Task<bool> update_record(TransactionScope tx, Record record) const {
        check_id<false>(record);
        const auto result = co_await write_record<false>(tx, update_, record);
        co_return result.affectedRows() != 0;
    }

    drogon::Task<> write_batch(TransactionScope tx, BatchCallback work) const {
        if (tx) return database_->savepoint(tx->get(), std::move(work));
        return database_->transaction(std::move(work));
    }

    drogon::Task<std::vector<Id>> create_batch(TransactionScope tx, std::vector<Record> records) const {
        std::vector<Id> ids;
        if (records.empty()) co_return ids;
        ids.reserve(records.size());
        co_await write_batch(tx, [&](const DaoTransaction& batch_tx) -> drogon::Task<> {
            for (auto& record : records) ids.push_back(co_await create(batch_tx, std::move(record)));
        });
        co_return ids;
    }

    struct UnmatchedBulkUpdate {};

    drogon::Task<bool> update_batch(TransactionScope tx, std::vector<Record> records) const {
        if (records.empty()) co_return true;
        try {
            co_await write_batch(tx, [&](const DaoTransaction& batch_tx) -> drogon::Task<> {
                for (auto& record : records) {
                    if (!co_await update(batch_tx, std::move(record))) throw UnmatchedBulkUpdate{};
                }
            });
        } catch (const UnmatchedBulkUpdate&) {
            co_return false;
        }
        co_return true;
    }

    drogon::Task<std::int64_t> delete_batch(TransactionScope tx, std::vector<Id> ids, bool soft) const {
        std::int64_t changed = 0;
        if (ids.empty()) co_return changed;
        co_await write_batch(tx, [&](const DaoTransaction& batch_tx) -> drogon::Task<> {
            for (auto& id : ids) {
                const auto deleted = soft ? co_await soft_delete_by_id(batch_tx, std::move(id))
                                          : co_await delete_by_id(batch_tx, std::move(id));
                if (deleted) ++changed;
            }
        });
        co_return changed;
    }

    drogon::Task<bool> delete_record(TransactionScope tx, Id id) const {
        const auto result = co_await execute(tx, "DELETE FROM " + table_ + " WHERE " + key_ + " = " + parameter(1),
                                            own_argument(std::move(id)));
        co_return result.affectedRows() != 0;
    }

    drogon::Task<bool> set_deleted(TransactionScope tx, Id id, bool deleted) const {
        const std::string value(deleted ? database_->current_timestamp_sql() : "NULL");
        const auto result = co_await execute(tx, "UPDATE " + table_ + " SET deleted_at = " + value +
            " WHERE " + key_ + " = " + parameter(1) +
            (deleted ? " AND deleted_at IS NULL" : " AND deleted_at IS NOT NULL"),
            own_argument(std::move(id)));
        co_return result.affectedRows() != 0;
    }

    template <typename... Predicates>
    drogon::Task<std::int64_t> count_records(TransactionScope tx, bool include, Predicates... predicates) const {
        std::string sql = "SELECT COUNT(*) FROM " + table_;
        std::vector<detail::QueryValue> values;
        append_where(sql, values, include, predicates...);
        const auto rows = co_await execute_bound(tx, std::move(sql), std::move(values));
        co_return rows[0][0].template as<std::int64_t>();
    }

    static constexpr std::array<std::string_view, 2> timestamp_names_{"created_at", "updated_at"};
    static constexpr auto names_ = glz::reflect<Fields>::keys;
    static constexpr auto indices_ = std::make_index_sequence<names_.size()>{};

    Record map_row(const drogon::orm::Row& row, std::vector<std::string> omitted = {}) const {
        if constexpr (!options.timestamps) {
            for (const auto name : timestamp_names_) omitted.emplace_back(name);
        }
        auto fields = row_to_struct<Fields>(row, omitted);
        Record result(std::move(fields), std::move(omitted));
        if (result.is_column_loaded("deleted_at")) {
            detail::read_row_field(result.deleted_at, row.at("deleted_at"), "deleted_at");
        }
        if constexpr (Record::generated_id) result.id = row.at("id").template as<Id>();
        if constexpr (options.timestamps) {
            if (result.is_column_loaded("created_at")) {
                detail::read_row_field(result.created_at, row.at("created_at"), "created_at");
            }
            if (result.is_column_loaded("updated_at")) {
                detail::read_row_field(result.updated_at, row.at("updated_at"), "updated_at");
            }
        }
        return result;
    }

    static constexpr bool timestamp_name(std::string_view name) {
        return std::ranges::find(timestamp_names_, name) != timestamp_names_.end();
    }

    template <std::size_t index>
    using Traits = detail::DaoFieldTraits<Fields, index>;

    template <std::size_t index>
    using Value = detail::Nullable<typename Traits<index>::Value>;

    template <std::size_t index>
    void add_field() {
        constexpr auto name = names_[index];
        static_assert(name != "deleted_at" && !timestamp_name(name) && (!Record::generated_id || name != "id"),
                      "reserved DAO fields are inherited");
        static_assert(!Traits<index>::foreign_key() || !Traits<index>::primary, "foreign fields cannot be primary keys");
        auto field = DatabaseColumn{std::string(name), detail::column_type<typename Value<index>::Type>(),
                           Value<index>::nullable, Traits<index>::primary, Traits<index>::unique, Traits<index>::indexed,
                           Traits<index>::default_metadata()};
        field.foreign_key = Traits<index>::foreign_key();
        using Scalar = typename Value<index>::Type;
        if constexpr (std::is_scoped_enum_v<Scalar>) {
            constexpr auto labels = magic_enum::enum_names<Scalar>();
            static_assert(!labels.empty(), "DAO enum has no reflectable values");
            field.enum_labels.assign(labels.begin(), labels.end());
            std::ranges::sort(field.enum_labels);
        }
        if (field.foreign_key && field.default_value) {
            throw std::invalid_argument("foreign fields cannot declare a default");
        }
        schema_.push_back(std::move(field));
    }

    template <std::size_t... index>
    void build_schema(std::index_sequence<index...>) {
        (add_field<index>(), ...);
        if constexpr (Record::generated_id) {
            schema_.push_back({"id", ColumnType::integer, false, true, false, false, {}, true});
        }
        schema_.push_back({"deleted_at", ColumnType::datetime, true, false, false, false, {}});
        if constexpr (options.timestamps) {
            for (const auto name : timestamp_names_) {
                DatabaseColumn field{std::string(name), ColumnType::datetime, true, false, false, true, {}, false, true};
                field.index_descending = true;
                schema_.push_back(std::move(field));
            }
        }
    }

    template <bool insert>
    static void check_id(const Record& record) {
        if constexpr (Record::generated_id) {
            if (record.id.has_value() == insert) {
                throw std::invalid_argument(insert ? "create requires an empty generated ID" :
                                             "update requires a generated ID");
            }
        }
    }

    struct FindQuery {
        EqualityFilters equality;
        std::vector<std::string_view> selected;
        std::vector<std::pair<std::string_view, SortDirection>> ordering;
        bool all = false;
        bool include_deleted = false;
    };

    static void append_column(std::string& list, std::string_view column) {
        if (!list.empty()) list += ", ";
        list += column;
    }

    static void require_enabled_column(std::string_view name) {
        if (!options.timestamps && timestamp_name(name)) {
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
        query.include_deleted = true;
        return std::tuple{};
    }

    static auto collect(FindQuery& query, detail::SelectAll) {
        query.all = true;
        return std::tuple{};
    }

    template <auto... members>
    static auto collect(FindQuery& query, detail::SelectFields<members...>) {
        ((require_enabled_column(glz::get_name<members>()),
          query.selected.push_back(glz::get_name<members>())), ...);
        return std::tuple{};
    }

    template <auto member>
    static auto collect(FindQuery& query, detail::OrderBy<member> order) {
        constexpr auto name = glz::get_name<member>();
        require_enabled_column(name);
        query.ordering.emplace_back(name, order.direction);
        return std::tuple{};
    }

    template <auto member>
    static auto collect(FindQuery&, detail::DaoPredicate<member>& predicate) {
        require_enabled_column(predicate.name);
        return std::tie(predicate);
    }

    template <auto first, auto second>
    static auto collect(FindQuery&, detail::DaoTupleLess<first, second>& predicate) {
        return std::tie(predicate);
    }

    template <auto member, typename Dao>
    static auto collect(FindQuery& query, detail::ForeignSelection<member, Dao>) {
        query.selected.push_back(glz::get_name<member>());
        return std::tuple{};
    }

    static auto foreign_selection(const auto&) { return std::tuple{}; }

    template <auto member, typename Dao>
    static auto foreign_selection(detail::ForeignSelection<member, Dao> selection) {
        return std::tuple{selection};
    }

    drogon::Task<std::map<Id, Record>> read_foreign_batch(
        TransactionScope tx, const std::set<Id>& keys) const {
        constexpr std::size_t batch_size = 500;
        const std::string key_name(Record::key_column);
        std::map<Id, Record> records;
        auto next = keys.begin();
        while (next != keys.end()) {
            std::string placeholders;
            std::vector<detail::QueryValue> values;
            while (next != keys.end() && values.size() < batch_size) {
                values.push_back(query_value(*next++));
                append_column(placeholders, parameter(values.size()));
            }
            auto rows = co_await execute_bound(tx, select_ + " WHERE " + key_ +
                " IN (" + placeholders + ") AND deleted_at IS NULL", std::move(values));
            for (const auto& row : rows) {
                auto key = detail::read_scalar<Id>(row.at(key_name));
                records.emplace(std::move(key), map_row(row));
            }
        }
        co_return records;
    }

    template <auto member, typename Dao>
    static drogon::Task<> load_foreign_page(TransactionScope tx, std::vector<Record>& records,
                                           detail::ForeignSelection<member, Dao> selection) {
        using TargetId = typename Dao::RecordType::Id;
        std::set<TargetId> keys;
        for (auto& record : records) {
            const std::optional<TargetId> key(detail::foreign_field(record.*member).key());
            if (key) keys.insert(*key);
        }
        if (keys.empty()) co_return;
        const auto targets = co_await selection.dao.get().read_foreign_batch(tx, keys);
        for (auto& record : records) {
            auto& field = detail::foreign_field(record.*member);
            const std::optional<TargetId> key(field.key());
            if (!key) continue;
            const auto target = targets.find(*key);
            if (target != targets.end()) field.set_record(target->second);
        }
    }

    template <typename... Modifiers>
    drogon::Task<std::vector<Record>> find_page(
        TransactionScope tx, std::int64_t limit, std::int64_t offset, Modifiers... modifiers) const {
        if (limit <= 0 || offset < 0) throw std::invalid_argument("invalid pagination");
        const auto foreign = std::tuple_cat(foreign_selection(modifiers)...);
        const auto same_backend = std::apply([&](const auto&... selections) {
            return (selections.dao.get().uses_backend(database_) && ...);
        }, foreign);
        if (!same_backend) throw std::invalid_argument("foreign DAOs must use the same database backend");

        FindQuery query;
        // Collect left to right so explicit sort fields retain the caller's order.
        auto parts = std::tuple<decltype(collect(query, modifiers))...>{collect(query, modifiers)...};
        const auto predicates = std::apply([](auto&&... part) {
            return std::tuple_cat(std::move(part)...);
        }, parts);

        std::string sql = select_;
        std::vector<std::string> omitted;
        if (!query.all) {
            auto projection = required_projection_;
            for (const auto& name : optional_columns_) {
                if (std::ranges::find(query.selected, name) == query.selected.end()) omitted.push_back(name);
                else append_column(projection, detail::quote_identifier(name));
            }
            sql = "SELECT " + projection + " FROM " + table_;
        }
        if (query.ordering.empty() && options.timestamps) {
            query.ordering.emplace_back("updated_at", SortDirection::desc);
        }
        if (std::ranges::none_of(query.ordering, [](const auto& item) { return item.first == Record::key_column; })) {
            query.ordering.emplace_back(Record::key_column, SortDirection::asc);
        }
        std::string ordering;
        for (const auto& [name, direction] : query.ordering) {
            append_column(ordering, detail::quote_identifier(name) +
                (direction == SortDirection::desc ? " DESC NULLS LAST" : " ASC NULLS LAST"));
        }
        std::vector<detail::QueryValue> values;
        std::apply([&](const auto&... predicates) { append_where(sql, values, query.include_deleted, predicates...); }, predicates);
        bool has_where = !query.include_deleted || std::tuple_size_v<decltype(predicates)> != 0;
        for (auto& [name, value] : query.equality.entries_) {
            require_enabled_column(name);
            sql += has_where ? " AND " : " WHERE ";
            has_where = true;
            values.push_back(std::move(value));
            sql += database_->null_safe_equal(detail::quote_identifier(name), values.size());
        }
        sql += " ORDER BY " + ordering + " LIMIT " + parameter(values.size() + 1) +
               " OFFSET " + parameter(values.size() + 2);
        values.emplace_back(limit);
        values.emplace_back(offset);
        const auto rows = co_await execute_bound(tx, std::move(sql), std::move(values));

        std::vector<Record> records;
        records.reserve(rows.size());
        for (const auto& row : rows) records.push_back(map_row(row, omitted));
        if constexpr (std::tuple_size_v<decltype(foreign)> > 0) {
            const auto load = [&](const auto&... selections) -> drogon::Task<> {
                (co_await load_foreign_page(tx, records, selections), ...);
            };
            co_await std::apply(load, foreign);
        }
        co_return records;
    }

    template <typename T>
    static detail::QueryValue query_value(const T& value) {
        auto owned = own_argument(value);
        using Owned = decltype(owned);
        if constexpr (detail::Nullable<Owned>::nullable) {
            if (!owned) return nullptr;
            return query_value(*owned);
        } else if constexpr (std::signed_integral<Owned>) {
            return static_cast<std::int64_t>(owned);
        } else {
            return owned;
        }
    }

    template <auto member>
    std::string predicate_sql(const detail::DaoPredicate<member>& predicate,
                             std::vector<detail::QueryValue>& values) const {
        using detail::PredicateOp;
        require_enabled_column(predicate.name);
        const auto column = detail::quote_identifier(predicate.name);
        std::string expression;
        if (predicate.operation == PredicateOp::in) {
            std::string list;
            bool includes_null = false;
            for (const typename detail::DaoPredicate<member>::Value value : predicate.values) {
                auto argument = query_value(value);
                if (std::holds_alternative<std::nullptr_t>(argument)) {
                    includes_null = true;
                } else {
                    values.push_back(std::move(argument));
                    append_column(list, parameter(values.size()));
                }
            }
            expression = list.empty() ? "FALSE" : column + " IN (" + list + ")";
            if (includes_null) expression += " OR " + column + " IS NULL";
        } else {
            values.push_back(query_value(static_cast<typename detail::DaoPredicate<member>::Value>(
                predicate.values.front())));
            const auto index = values.size();
            switch (predicate.operation) {
                case PredicateOp::eq: expression = database_->null_safe_equal(column, index); break;
                case PredicateOp::gt: expression = column + " > " + parameter(index); break;
                case PredicateOp::gte: expression = column + " >= " + parameter(index); break;
                case PredicateOp::lt: expression = column + " < " + parameter(index); break;
                case PredicateOp::lte: expression = column + " <= " + parameter(index); break;
                case PredicateOp::contains: expression = database_->contains_text(column, index); break;
                case PredicateOp::in: break;
            }
        }
        if (predicate.negated) return "NOT COALESCE((" + expression + "), FALSE)";
        return "(" + expression + ")";
    }

    template <auto first, auto second>
    std::string predicate_sql(const detail::DaoTupleLess<first, second>& predicate,
                             std::vector<detail::QueryValue>& values) const {
        constexpr auto first_column = glz::get_name<first>();
        constexpr auto second_column = glz::get_name<second>();
        require_enabled_column(first_column);
        require_enabled_column(second_column);
        const auto first_parameter = parameter(values.size() + 1);
        const auto second_parameter = parameter(values.size() + 2);
        values.push_back(query_value(predicate.first));
        values.push_back(query_value(predicate.second));
        return "(" + detail::quote_identifier(first_column) + ", " + detail::quote_identifier(second_column) +
               ") < (" + first_parameter + ", " + second_parameter + ")";
    }

    template <typename... Predicates>
    void append_where(std::string& sql, std::vector<detail::QueryValue>& values, bool include,
                     const Predicates&... predicates) const {
        if (!include) sql += " WHERE deleted_at IS NULL";
        if constexpr (sizeof...(Predicates) > 0) {
            auto separator = include ? " WHERE " : " AND ";
            auto append = [&](const auto& predicate) {
                sql += separator + predicate_sql(predicate, values);
                separator = " AND ";
            };
            (append(predicates), ...);
        }
    }

    drogon::Task<drogon::orm::Result> execute_bound(
        TransactionScope tx, std::string sql, std::vector<detail::QueryValue> values) const {
        if (tx) {
            co_return co_await detail::execute_bound_on(transaction_client(tx->get()), std::move(sql), std::move(values));
        }
        if (const auto gate = database_->query_gate()) {
            const auto lock = co_await gate->get().scoped_lock();
            co_return co_await detail::execute_bound_on(database_->client(), std::move(sql), std::move(values));
        }
        co_return co_await detail::execute_bound_on(database_->client(), std::move(sql), std::move(values));
    }

    template <std::size_t index, typename Field>
    static decltype(auto) update_value(const Record& record, Field& field) {
        if constexpr (Value<index>::nullable) {
            using Argument = typename Traits<index>::Value;
            return record.is_column_loaded(names_[index]) ? Argument(Traits<index>::unwrap(field)) : Argument{};
        } else {
            return Traits<index>::unwrap(field);
        }
    }

    template <std::size_t index>
    static auto update_flag(const Record& record) {
        if constexpr (Value<index>::nullable) return std::tuple{record.is_column_loaded(names_[index])};
        else return std::tuple{};
    }

    template <bool insert>
    drogon::Task<drogon::orm::Result> write_record(TransactionScope tx, const std::string& sql, Record& record) const {
        auto fields = glz::to_tie(static_cast<Fields&>(record));
        return [&]<std::size_t... index>(std::index_sequence<index...>) {
            if constexpr (insert) {
                return execute(tx, sql, own_argument(Traits<index>::unwrap(glz::get<index>(fields)))...);
            } else {
                return std::apply([&](auto... flags) {
                    if constexpr (Record::generated_id) {
                        return execute(tx, sql, own_argument(update_value<index>(record, glz::get<index>(fields)))..., *record.id, flags...);
                    } else {
                        return execute(tx, sql, own_argument(update_value<index>(record, glz::get<index>(fields)))..., flags...);
                    }
                }, std::tuple_cat(update_flag<index>(record)...));
            }
        }(indices_);
    }

    template <typename T>
    static auto own_argument(T&& value) {
        using Argument = std::remove_cvref_t<T>;
        using Scalar = typename detail::Nullable<Argument>::Type;
        if constexpr (std::is_scoped_enum_v<Argument>) {
            return std::string(detail::enum_label(value));
        } else if constexpr (std::is_scoped_enum_v<Scalar>) {
            return value ? std::optional{std::string(detail::enum_label(*value))} : std::nullopt;
        } else if constexpr (std::same_as<Argument, DaoDateTime>) {
            return detail::format_dao_date_time(value);
        } else if constexpr (std::same_as<std::remove_cvref_t<T>, std::optional<DaoDateTime>>) {
            return value ? std::optional{detail::format_dao_date_time(*value)} : std::nullopt;
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
            co_return co_await detail::execute_on(transaction_client(tx->get()), std::move(sql), std::move(args)...);
        }
        if (const auto gate = database_->query_gate()) {
            const auto lock = co_await gate->get().scoped_lock();
            co_return co_await detail::execute_on(database_->client(), std::move(sql), std::move(args)...);
        }
        co_return co_await detail::execute_on(database_->client(), std::move(sql), std::move(args)...);
    }

    DatabaseBackendPtr database_;
    std::string table_name_, table_, key_, select_, insert_, update_;
    std::vector<DatabaseColumn> schema_;
    std::vector<std::string> optional_columns_;
    std::string required_projection_;
};

}
