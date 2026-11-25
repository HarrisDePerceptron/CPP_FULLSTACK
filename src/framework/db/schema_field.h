#pragma once

#include "dao_datetime.h"

#include <magic_enum/magic_enum.hpp>

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace APP_NAMESPACE {

enum class ColumnType { text, integer, boolean, real, datetime };

using ColumnDefault = std::variant<std::string, std::int64_t, bool, double, DaoDateTime>;

struct ForeignKey {
    std::string_view table, column;
};

struct DatabaseColumn {
    std::string name;
    ColumnType type;
    bool nullable, primary, unique, indexed;
    std::optional<ColumnDefault> default_value;
    bool generated = false;
    bool timestamp = false;
    std::optional<ForeignKey> foreign_key;
    std::vector<std::string_view> enum_labels;
    bool index_descending = false;
};

enum class ColumnConstraint { primary, unique, indexed };

template <typename T, ColumnConstraint constraint>
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

template <std::size_t size>
struct SqlText {
    char chars[size]{};

    constexpr SqlText(const char (&text)[size]) {
        for (std::size_t i = 0; i < size; ++i) chars[i] = text[i];
    }

    constexpr std::string_view view() const { return {chars, size - 1}; }
};

namespace detail {

template <typename T> struct Nullable {
    using Type = T;
    static constexpr bool nullable = false;
};
template <typename T> struct Nullable<std::optional<T>> {
    using Type = T;
    static constexpr bool nullable = true;
};

template <typename E>
constexpr std::string_view enum_label(E value) {
    const auto label = magic_enum::enum_name(value);
    if (label.empty()) throw std::invalid_argument("unrecognized DAO enum value");
    return label;
}

template <typename T, auto initial>
auto default_value() {
    using Scalar = typename Nullable<T>::Type;
    if constexpr (std::same_as<Scalar, std::string>) {
        constexpr auto text = initial.view();
        static_assert(text.find('\0') == std::string_view::npos, "SQL text defaults cannot contain NUL");
        return std::string(text);
    } else if constexpr (std::is_scoped_enum_v<Scalar>) {
        static_assert(std::same_as<decltype(initial), Scalar>, "enum default must match the field type");
        static_assert(!enum_label(initial).empty());
        return Scalar(initial);
    } else if constexpr (std::same_as<Scalar, DaoDateTime>) {
        constexpr auto datetime = parse_dao_date_time(initial.view());
        return datetime;
    } else if constexpr (std::same_as<Scalar, bool>) {
        static_assert(std::same_as<decltype(initial), bool>, "boolean defaults require true or false");
        return initial;
    } else if constexpr (std::signed_integral<Scalar>) {
        static_assert(std::signed_integral<decltype(initial)> && std::in_range<Scalar>(initial),
                      "integer default must fit the field");
        return static_cast<Scalar>(initial);
    } else {
        static_assert(std::same_as<Scalar, double> && std::same_as<decltype(initial), double>,
                      "real defaults require a double template argument");
        constexpr double number = initial;
        static_assert(number >= -std::numeric_limits<double>::max() &&
                      number <= std::numeric_limits<double>::max(), "default must be finite");
        return number;
    }
}


}

template <typename T, auto initial>
struct Default {
    T value = detail::default_value<T, initial>();

    Default() = default;
    template <typename U>
        requires (!std::same_as<std::remove_cvref_t<U>, Default>) && std::constructible_from<T, U>
    Default(U&& input) : value(std::forward<U>(input)) {}
};

namespace detail {

template <typename T>
struct FieldTraits {
    using Value = T;
    static constexpr bool primary = false, unique = false, indexed = false;
    static T& unwrap(T& field) { return field; }
    static std::optional<ColumnDefault> default_metadata() { return std::nullopt; }
    static constexpr std::optional<ForeignKey> foreign_key() { return std::nullopt; }
};

template <typename T, auto initial>
struct FieldTraits<Default<T, initial>> : FieldTraits<T> {
    static T& unwrap(Default<T, initial>& field) { return field.value; }
    static std::optional<ColumnDefault> default_metadata() { auto value = default_value<T, initial>();
        if constexpr (std::is_scoped_enum_v<decltype(value)>) {
            return ColumnDefault{std::string(enum_label(value))};
        } else if constexpr (std::signed_integral<decltype(value)>) {
            return ColumnDefault{static_cast<std::int64_t>(value)};
        } else {
            return ColumnDefault{std::move(value)};
        } }
};

template <typename T, ColumnConstraint constraint>
struct FieldTraits<SchemaField<T, constraint>> : FieldTraits<T> {
    using Inner = FieldTraits<T>;
    static constexpr bool primary = Inner::primary || constraint == ColumnConstraint::primary;
    static constexpr bool unique = Inner::unique || constraint == ColumnConstraint::unique;
    static constexpr bool indexed = Inner::indexed || constraint == ColumnConstraint::indexed;
    static auto& unwrap(SchemaField<T, constraint>& field) { return Inner::unwrap(field.value); }
    static void set_key(SchemaField<T, constraint>& field, typename Inner::Value key) {
        Inner::set_key(field.value, std::move(key));
    }
};

template <typename T>
constexpr ColumnType column_type() {
    if constexpr (std::same_as<T, std::string> || std::is_scoped_enum_v<T>) return ColumnType::text;
    else if constexpr (std::same_as<T, bool>) return ColumnType::boolean;
    else if constexpr (std::signed_integral<T>) return ColumnType::integer;
    else if constexpr (std::same_as<T, double>) return ColumnType::real;
    else if constexpr (std::same_as<T, DaoDateTime>) return ColumnType::datetime;
    else static_assert(!sizeof(T), "unsupported database field type");
}

}

}
