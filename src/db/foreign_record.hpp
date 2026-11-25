#pragma once

#include "dao_record.hpp"

#include <drogon/utils/coroutine.h>

#include <functional>

namespace APP_NAMESPACE {

template <typename Record, bool Nullable = true>
class Foreign {
public:
    using TargetRecord = Record;
    using Key = std::conditional_t<Nullable, std::optional<typename Record::Id>, typename Record::Id>;

    Foreign() = default;
    template <typename U>
        requires (!std::same_as<std::remove_cvref_t<U>, Foreign>) && std::constructible_from<Key, U>
    Foreign(U&& key) : key_(std::forward<U>(key)) {}

    const Key& key() const { return key_; }
    const std::optional<Record>& record() const { return record_; }

    void setRecord(const Record& record) { record_ = record; }

    void setKey(Key key) {
        key_ = std::move(key);
        record_.reset();
    }

    template <typename Dao, typename... Context>
    drogon::Task<bool> load(const Dao& dao, const Context&... context) {
        record_.reset();
        if constexpr (Nullable) {
            if (!key_) co_return false;
            record_ = co_await dao.findById(context..., *key_);
        } else {
            record_ = co_await dao.findById(context..., key_);
        }
        co_return record_.has_value();
    }

private:
    Key key_{};
    std::optional<Record> record_;
};

template <typename Record>
using RequiredForeign = Foreign<Record, false>;

namespace detail {

template <typename T>
struct ForeignField;

template <typename Record, bool Nullable>
struct ForeignField<Foreign<Record, Nullable>> {
    static Foreign<Record, Nullable>& get(Foreign<Record, Nullable>& field) { return field; }
};

template <typename T, ColumnConstraint Constraint>
    requires requires(T& field) { ForeignField<T>::get(field); }
struct ForeignField<SchemaField<T, Constraint>> {
    static decltype(auto) get(SchemaField<T, Constraint>& field) {
        return ForeignField<T>::get(field.value);
    }
};

template <typename T>
    requires requires(T& field) { ForeignField<T>::get(field); }
decltype(auto) foreignField(T& field) { return ForeignField<T>::get(field); }

template <auto Member, typename Dao>
struct ForeignSelection {
    std::reference_wrapper<const Dao> dao;
};

template <auto Member, typename Source, typename Dao>
concept ForeignSelectionFor = requires(Source& source) {
    requires std::same_as<typename std::remove_reference_t<decltype(
        foreignField(source.*Member))>::TargetRecord, typename Dao::RecordType>;
};

template <typename Record, bool Nullable>
struct FieldTraits<Foreign<Record, Nullable>> {
    using Field = Foreign<Record, Nullable>;
    using Value = typename Field::Key;
    static constexpr bool primary = false, unique = false, indexed = false;

    static const Value& unwrap(const Field& field) { return field.key(); }
    static void setKey(Field& field, Value key) { field.setKey(std::move(key)); }
    static std::optional<ColumnDefault> defaultMetadata() { return std::nullopt; }
    static constexpr std::optional<ForeignKey> foreignKey() {
        return ForeignKey{Record::table_name, Record::keyColumn};
    }
};

}

template <auto Member, typename Dao>
auto withForeign(Dao& dao) {
    return detail::ForeignSelection<Member, std::remove_cv_t<Dao>>{std::cref(dao)};
}

}
