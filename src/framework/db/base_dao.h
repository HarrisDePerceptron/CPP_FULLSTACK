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

enum class SortDirection { asc,
                           desc };

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
                if (names[i] == names[j])
                    return false;
            }
        }
        return true;
    }
}

}

inline auto include_deleted() { return detail::IncludeDeleted{}; }

template <auto... members>
    requires(sizeof...(members) >= 2) &&
            (std::is_member_object_pointer_v<decltype(members)> && ...) &&
            (detail::valid_composite_members<members...>())
struct CompositeUnique {};

struct DaoOptions {
    bool timestamps = true;
};

template <typename Derived, typename Record, DaoOptions options = {}>
class BaseDao {
    template <typename, typename, DaoOptions>
    friend class BaseDao;

    using Fields = typename Record::FieldRecord;
    using Id = typename Record::Id;
    using TransactionScope = std::optional<std::reference_wrapper<const DaoTransaction>>;

    struct PatchAssignment {
        std::string_view column;
        detail::QueryValue value;
    };

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
        if constexpr (model_member<member>)
            return !detail::FieldTraits<ModelField<member>>::primary;
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
    class NamedFilters;

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
    class FindBuilder;

    FindBuilder<> find() const&;
    FindBuilder<> find(const DaoTransaction& tx) const&;
    FindBuilder<> find() const&& = delete;
    FindBuilder<> find(const DaoTransaction&) const&& = delete;

    drogon::Task<> transaction(TransactionCallback work) const& {
        return database_->transaction(std::move(work));
    }

    drogon::Task<> transaction(TransactionCallback work) const&& = delete;

    template <bool nullable>
    drogon::Task<bool> load_foreign(Foreign<Record, nullable>& field) const& {
        return field.load(*this);
    }

    template <bool nullable>
    drogon::Task<bool> load_foreign(Foreign<Record, nullable>& field) const&& = delete;

    template <bool nullable>
    drogon::Task<bool> load_foreign(const DaoTransaction& tx, Foreign<Record, nullable>& field) const& {
        transaction_client(tx);
        return field.load(*this, tx);
    }

    template <bool nullable>
    drogon::Task<bool> load_foreign(const DaoTransaction& tx, Foreign<Record, nullable>& field) const&& = delete;

    template <typename... Selections>
    class FindByIdBuilder;

    FindByIdBuilder<> find_by_id(Id id) const&;

    FindByIdBuilder<> find_by_id(const DaoTransaction& tx, Id id) const&;

    FindByIdBuilder<> find_by_id(Id) const&& = delete;
    FindByIdBuilder<> find_by_id(const DaoTransaction&, Id) const&& = delete;

    bool uses_backend(const DatabaseBackendPtr& database) const {
        return database_ == database;
    }

    drogon::Task<Id> create(Record record) const& {
        return create_record({}, std::move(record));
    }

    drogon::Task<Id> create(Record record) const&& = delete;

    drogon::Task<Id> create(const DaoTransaction& tx, Record record) const& {
        return create_record(std::cref(tx), std::move(record));
    }

    drogon::Task<Id> create(const DaoTransaction& tx, Record record) const&& = delete;

    template <auto conflict_key>
        requires(conflict_member<conflict_key>)
    drogon::Task<bool> insert_if_absent(Record record) const& {
        return insert_absent<conflict_key>({}, std::move(record));
    }

    template <auto conflict_key>
        requires(conflict_member<conflict_key>)
    drogon::Task<bool> insert_if_absent(const DaoTransaction& tx, Record record) const& {
        return insert_absent<conflict_key>(std::cref(tx), std::move(record));
    }

    template <auto conflict_key>
    drogon::Task<bool> insert_if_absent(Record) const&& = delete;
    template <auto conflict_key>
    drogon::Task<bool> insert_if_absent(const DaoTransaction&, Record) const&& = delete;

    template <auto conflict_key, auto... members>
        requires(sizeof...(members) > 0) && (conflict_member<conflict_key>) &&
                (writable_member<members> && ...) && (detail::valid_composite_members<conflict_key, members...>())
    drogon::Task<Id> upsert(Record record) const& {
        return upsert_record<conflict_key, members...>({}, std::move(record));
    }

    template <auto conflict_key, auto... members>
        requires(sizeof...(members) > 0) && (conflict_member<conflict_key>) &&
                (writable_member<members> && ...) && (detail::valid_composite_members<conflict_key, members...>())
    drogon::Task<Id> upsert(const DaoTransaction& tx, Record record) const& {
        return upsert_record<conflict_key, members...>(std::cref(tx), std::move(record));
    }

    template <auto conflict_key, auto... members>
    drogon::Task<Id> upsert(Record) const&& = delete;
    template <auto conflict_key, auto... members>
    drogon::Task<Id> upsert(const DaoTransaction&, Record) const&& = delete;

    class UpdateByIdBuilder;

    UpdateByIdBuilder update_by_id(Id id) const&;

    UpdateByIdBuilder update_by_id(const DaoTransaction& tx, Id id) const&;

    UpdateByIdBuilder update_by_id(Id) const&& = delete;
    UpdateByIdBuilder update_by_id(const DaoTransaction&, Id) const&& = delete;

    drogon::Task<bool> update(Record record) const& {
        return update_record({}, std::move(record));
    }

    drogon::Task<bool> update(Record record) const&& = delete;

    drogon::Task<bool> update(const DaoTransaction& tx, Record record) const& {
        return update_record(std::cref(tx), std::move(record));
    }

    drogon::Task<bool> update(const DaoTransaction& tx, Record record) const&& = delete;

    drogon::Task<std::vector<Id>> create_many(std::vector<Record> records) const& {
        return create_batch({}, std::move(records));
    }

    drogon::Task<std::vector<Id>> create_many(std::vector<Record> records) const&& = delete;

    drogon::Task<std::vector<Id>> create_many(const DaoTransaction& tx, std::vector<Record> records) const& {
        return create_batch(std::cref(tx), std::move(records));
    }

    drogon::Task<std::vector<Id>> create_many(const DaoTransaction& tx, std::vector<Record> records) const&& = delete;

    drogon::Task<bool> update_many(std::vector<Record> records) const& {
        return update_batch({}, std::move(records));
    }

    drogon::Task<bool> update_many(std::vector<Record> records) const&& = delete;

    drogon::Task<bool> update_many(const DaoTransaction& tx, std::vector<Record> records) const& {
        return update_batch(std::cref(tx), std::move(records));
    }

    drogon::Task<bool> update_many(const DaoTransaction& tx, std::vector<Record> records) const&& = delete;

    drogon::Task<bool> delete_by_id(Id id) const& {
        return delete_record({}, std::move(id));
    }

    drogon::Task<bool> delete_by_id(Id id) const&& = delete;

    drogon::Task<bool> delete_by_id(const DaoTransaction& tx, Id id) const& {
        return delete_record(std::cref(tx), std::move(id));
    }

    drogon::Task<bool> delete_by_id(const DaoTransaction& tx, Id id) const&& = delete;

    drogon::Task<bool> soft_delete_by_id(Id id) const& {
        return set_deleted({}, std::move(id), true);
    }

    drogon::Task<bool> soft_delete_by_id(Id id) const&& = delete;

    drogon::Task<bool> soft_delete_by_id(const DaoTransaction& tx, Id id) const& {
        return set_deleted(std::cref(tx), std::move(id), true);
    }

    drogon::Task<bool> soft_delete_by_id(const DaoTransaction& tx, Id id) const&& = delete;

    drogon::Task<bool> restore_by_id(Id id) const& {
        return set_deleted({}, std::move(id), false);
    }

    drogon::Task<bool> restore_by_id(Id id) const&& = delete;

    drogon::Task<bool> restore_by_id(const DaoTransaction& tx, Id id) const& {
        return set_deleted(std::cref(tx), std::move(id), false);
    }

    drogon::Task<bool> restore_by_id(const DaoTransaction& tx, Id id) const&& = delete;

    drogon::Task<std::int64_t> delete_many(std::vector<Id> ids) const& {
        return delete_batch({}, std::move(ids), false);
    }

    drogon::Task<std::int64_t> delete_many(std::vector<Id> ids) const&& = delete;

    drogon::Task<std::int64_t> delete_many(const DaoTransaction& tx, std::vector<Id> ids) const& {
        return delete_batch(std::cref(tx), std::move(ids), false);
    }

    drogon::Task<std::int64_t> delete_many(const DaoTransaction& tx, std::vector<Id> ids) const&& = delete;

    drogon::Task<std::int64_t> soft_delete_many(std::vector<Id> ids) const& {
        return delete_batch({}, std::move(ids), true);
    }

    drogon::Task<std::int64_t> soft_delete_many(std::vector<Id> ids) const&& = delete;

    drogon::Task<std::int64_t> soft_delete_many(const DaoTransaction& tx, std::vector<Id> ids) const& {
        return delete_batch(std::cref(tx), std::move(ids), true);
    }

    drogon::Task<std::int64_t> soft_delete_many(const DaoTransaction& tx, std::vector<Id> ids) const&& = delete;

    template <typename... Predicates>
    class CountBuilder;

    CountBuilder<> count() const&;
    CountBuilder<> count(const DaoTransaction& tx) const&;
    CountBuilder<> count() const&& = delete;
    CountBuilder<> count(const DaoTransaction&) const&& = delete;

    // Use application-owned SQL and pass runtime values as bound parameters.
    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(std::string sql, Args&&... args) const& {
        return execute({}, std::move(sql), own_argument(std::forward<Args>(args))...);
    }

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(std::string sql, Args&&... args) const&& = delete;

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(const DaoTransaction& tx, std::string sql, Args&&... args) const& {
        return execute(std::cref(tx), std::move(sql), own_argument(std::forward<Args>(args))...);
    }

    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(const DaoTransaction& tx, std::string sql, Args&&... args) const&& = delete;

  protected:
    static drogon::Task<Derived> finish_initialization(Derived dao);

    BaseDao(const BaseDao&) = delete;
    BaseDao& operator=(const BaseDao&) = delete;
    BaseDao(BaseDao&&) noexcept = default;
    BaseDao& operator=(BaseDao&&) noexcept = default;

    explicit BaseDao(DatabaseBackendPtr database);

    std::string parameter(std::size_t index) const { return database_->parameter(index); }

  private:
    template <auto... members>
    static void add_composite_unique(CompositeUniqueness& constraints, CompositeUnique<members...>);

    template <typename... Constraints>
    static void add_composite_unique(CompositeUniqueness& result, const std::tuple<Constraints...>& constraints);

    static CompositeUniqueness composite_uniqueness();

    const drogon::orm::DbClientPtr& transaction_client(const DaoTransaction& tx) const {
        if (std::addressof(tx.backend()) != database_.get()) {
            throw std::invalid_argument("DAO and transaction must use the same database backend");
        }
        return tx.client();
    }

    drogon::Task<std::optional<Record>> read_by_id(TransactionScope tx, Id id, bool include = false) const;

    template <auto... members, typename... TargetDaos>
    drogon::Task<std::optional<Record>> read_with_foreign(
        TransactionScope tx, Id id, bool include, detail::ForeignSelection<members, TargetDaos>... selections) const;

    drogon::Task<Id> create_record(TransactionScope tx, Record record) const;

    template <auto conflict_key>
    static std::string conflict_target();

    template <auto conflict_key>
    drogon::Task<bool> insert_absent(TransactionScope tx, Record record) const;

    template <auto conflict_key, auto... members>
    drogon::Task<Id> upsert_record(TransactionScope tx, Record record) const;

    drogon::Task<bool> patch_record(TransactionScope tx, Id id,
                                    std::vector<PatchAssignment> fields) const;

    drogon::Task<bool> update_record(TransactionScope tx, Record record) const;

    drogon::Task<> write_batch(TransactionScope tx, BatchCallback work) const;

    drogon::Task<std::vector<Id>> create_batch(TransactionScope tx, std::vector<Record> records) const;

    struct UnmatchedBulkUpdate {};

    drogon::Task<bool> update_batch(TransactionScope tx, std::vector<Record> records) const;

    drogon::Task<std::int64_t> delete_batch(TransactionScope tx, std::vector<Id> ids, bool soft) const;

    drogon::Task<bool> delete_record(TransactionScope tx, Id id) const;

    drogon::Task<bool> set_deleted(TransactionScope tx, Id id, bool deleted) const;

    template <typename... Predicates>
    drogon::Task<std::int64_t> count_records(TransactionScope tx, bool include, Predicates... predicates) const;

    static constexpr std::array<std::string_view, 2> timestamp_names_{"created_at", "updated_at"};
    static constexpr auto names_ = glz::reflect<Fields>::keys;
    static constexpr auto indices_ = std::make_index_sequence<names_.size()>{};

    Record map_row(const drogon::orm::Row& row, std::vector<std::string> omitted = {}) const;

    static constexpr bool timestamp_name(std::string_view name);

    template <std::size_t index>
    using Traits = detail::DaoFieldTraits<Fields, index>;

    template <std::size_t index>
    using Value = detail::Nullable<typename Traits<index>::Value>;

    template <std::size_t index>
    void add_field();

    template <std::size_t... index>
    void build_schema(std::index_sequence<index...>);

    template <bool insert>
    static void check_id(const Record& record);

    struct FindQuery {
        EqualityFilters equality;
        std::vector<std::string_view> selected;
        std::vector<std::pair<std::string_view, SortDirection>> ordering;
        bool all = false;
        bool include_deleted = false;
    };

    static void append_column(std::string& list, std::string_view column) {
        if (!list.empty())
            list += ", ";
        list += column;
    }

    static void require_enabled_column(std::string_view name);

    static auto collect(FindQuery& query, EqualityFilters& filters);

    static auto collect(FindQuery& query, detail::IncludeDeleted);

    static auto collect(FindQuery& query, detail::SelectAll);

    template <auto... members>
    static auto collect(FindQuery& query, detail::SelectFields<members...>);

    template <auto member>
    static auto collect(FindQuery& query, detail::OrderBy<member> order);

    template <auto member>
    static auto collect(FindQuery&, detail::DaoPredicate<member>& predicate);

    template <auto first, auto second>
    static auto collect(FindQuery&, detail::DaoTupleLess<first, second>& predicate);

    template <auto member, typename Dao>
    static auto collect(FindQuery& query, detail::ForeignSelection<member, Dao>);

    static auto foreign_selection(const auto&) { return std::tuple{}; }

    template <auto member, typename Dao>
    static auto foreign_selection(detail::ForeignSelection<member, Dao> selection) {
        return std::tuple{selection};
    }

    drogon::Task<std::map<Id, Record>> read_foreign_batch(
        TransactionScope tx, const std::set<Id>& keys) const;

    template <auto member, typename Dao>
    static drogon::Task<> load_foreign_page(TransactionScope tx, std::vector<Record>& records,
                                            detail::ForeignSelection<member, Dao> selection);

    template <typename... Modifiers>
    drogon::Task<std::vector<Record>> find_page(
        TransactionScope tx, std::int64_t limit, std::int64_t offset, Modifiers... modifiers) const;

    template <typename T>
    static detail::QueryValue query_value(const T& value) {
        auto owned = own_argument(value);
        using Owned = decltype(owned);
        if constexpr (detail::Nullable<Owned>::nullable) {
            if (!owned)
                return nullptr;
            return query_value(*owned);
        } else if constexpr (std::signed_integral<Owned>) {
            return static_cast<std::int64_t>(owned);
        } else {
            return owned;
        }
    }

    template <auto member>
    std::string predicate_sql(const detail::DaoPredicate<member>& predicate,
                              std::vector<detail::QueryValue>& values) const;

    template <auto first, auto second>
    std::string predicate_sql(const detail::DaoTupleLess<first, second>& predicate,
                              std::vector<detail::QueryValue>& values) const;

    template <typename... Predicates>
    void append_where(std::string& sql, std::vector<detail::QueryValue>& values, bool include,
                      const Predicates&... predicates) const;

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
    static decltype(auto) update_value(const Record& record, Field& field);

    template <std::size_t index>
    static auto update_flag(const Record& record);

    template <bool insert>
    drogon::Task<drogon::orm::Result> write_record(TransactionScope tx, const std::string& sql, Record& record) const;

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
        } else if constexpr (std::is_convertible_v<T, std::string_view> && !std::is_same_v<std::decay_t<T>, std::nullptr_t>) {
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

#include "framework/db/detail/dao_builders.h"
#include "framework/db/detail/dao_reads.h"
#include "framework/db/detail/dao_writes.h"
#include "framework/db/detail/dao_schema.h"
