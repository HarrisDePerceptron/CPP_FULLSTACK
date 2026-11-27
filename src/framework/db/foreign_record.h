#pragma once

#include "dao_record.h"

#include <drogon/utils/coroutine.h>

#include <functional>

namespace APP_NAMESPACE {

template <typename Record, bool nullable = true> class Foreign {
  public:
    using TargetRecord = Record;
    using Key = std::conditional_t<nullable, std::optional<typename Record::Id>,
                                   typename Record::Id>;

    Foreign() = default;
    template <typename U>
        requires(!std::same_as<std::remove_cvref_t<U>, Foreign>) &&
                std::constructible_from<Key, U>
    Foreign(U&& key) : key_(std::forward<U>(key)) {}

    const Key& key() const { return key_; }
    const std::optional<Record>& record() const { return record_; }

    void set_record(const Record& record) { record_ = record; }

    void set_key(Key key) {
        key_ = std::move(key);
        record_.reset();
    }

    template <typename Dao, typename... Context>
    drogon::Task<bool> load(const Dao& dao, const Context&... context) {
        record_.reset();
        if constexpr (nullable) {
            if (!key_)
                co_return false;
            record_ = co_await dao.find_by_id(context..., *key_).collect();
        } else {
            record_ = co_await dao.find_by_id(context..., key_).collect();
        }
        co_return record_.has_value();
    }

  private:
    Key key_{};
    std::optional<Record> record_;
};

template <typename Record> using RequiredForeign = Foreign<Record, false>;

namespace detail {

template <typename T> struct ForeignField;

template <typename Record, bool nullable>
struct ForeignField<Foreign<Record, nullable>> {
    static Foreign<Record, nullable>& get(Foreign<Record, nullable>& field) {
        return field;
    }
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
decltype(auto) foreign_field(T& field) {
    return ForeignField<T>::get(field);
}

template <auto member, typename Dao> struct ForeignSelection {
    std::reference_wrapper<const Dao> dao;
};

template <auto member, typename Source, typename Dao>
concept ForeignSelectionFor = requires(Source& source) {
    requires std::same_as<
        typename std::remove_reference_t<decltype(foreign_field(
            source.*member))>::TargetRecord,
        typename Dao::RecordType>;
};

template <typename Record, bool nullable>
struct FieldTraits<Foreign<Record, nullable>> {
    using Field = Foreign<Record, nullable>;
    using Value = typename Field::Key;
    static constexpr bool primary = false, unique = false, indexed = false;

    static const Value& unwrap(const Field& field) { return field.key(); }
    static void set_key(Field& field, Value key) {
        field.set_key(std::move(key));
    }
    static std::optional<ColumnDefault> default_metadata() {
        return std::nullopt;
    }
    static constexpr std::optional<ForeignKey> foreign_key() {
        return ForeignKey{Record::table_name, Record::key_column};
    }
};

}

template <auto member, typename Dao> auto with_foreign(Dao& dao) {
    return detail::ForeignSelection<member, std::remove_cv_t<Dao>>{
        std::cref(dao)};
}

}
