#pragma once

#include "app_namespace.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace APP_NAMESPACE {

enum class ColumnConstraint { primary, unique, indexed };

template <typename T, ColumnConstraint Constraint>
struct SchemaField {
    T value{};

    SchemaField() = default;
    template <typename U>
        requires (!std::same_as<std::remove_cvref_t<U>, SchemaField>)
            && std::constructible_from<T, U>
    SchemaField(U&& input) : value(std::forward<U>(input)) {}
};

template <typename T> using Primary = SchemaField<T, ColumnConstraint::primary>;
template <typename T> using Unique = SchemaField<T, ColumnConstraint::unique>;
template <typename T> using Indexed = SchemaField<T, ColumnConstraint::indexed>;

namespace detail {

template <typename T> struct Nullable {
    using Type = T;
    static constexpr bool nullable = false;
};
template <typename T> struct Nullable<std::optional<T>> {
    using Type = T;
    static constexpr bool nullable = true;
};

template <typename T>
struct FieldTraits {
    using Value = T;
    static constexpr bool primary = false, unique = false, indexed = false;
    static T& unwrap(T& field) { return field; }
};

template <typename T, ColumnConstraint Constraint>
struct FieldTraits<SchemaField<T, Constraint>> {
    using Value = T;
    static constexpr bool primary = Constraint == ColumnConstraint::primary;
    static constexpr bool unique = Constraint == ColumnConstraint::unique;
    static constexpr bool indexed = Constraint == ColumnConstraint::indexed;
    static T& unwrap(SchemaField<T, Constraint>& field) { return field.value; }
};

template <typename T>
constexpr std::string_view sqliteType() {
    if constexpr (std::same_as<T, std::string>) return "TEXT";
    else if constexpr (std::signed_integral<T> || std::same_as<T, bool>) return "INTEGER";
    else if constexpr (std::same_as<T, double>) return "REAL";
    else static_assert(!sizeof(T), "unsupported SQLite field type");
}

}

}
