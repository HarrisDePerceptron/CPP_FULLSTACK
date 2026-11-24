#pragma once

#include "dao_datetime.hpp"

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
    std::optional<ColumnDefault> defaultValue;
    bool generated = false;
    bool timestamp = false;
    std::optional<ForeignKey> foreignKey;
    std::vector<std::string_view> enumLabels;
    bool indexDescending = false;
};

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

template <std::size_t N>
struct SqlText {
    char chars[N]{};

    constexpr SqlText(const char (&text)[N]) {
        for (std::size_t i = 0; i < N; ++i) chars[i] = text[i];
    }

    constexpr std::string_view view() const { return {chars, N - 1}; }
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
constexpr std::string_view enumLabel(E value) {
    const auto label = magic_enum::enum_name(value);
    if (label.empty()) throw std::invalid_argument("unrecognized DAO enum value");
    return label;
}

template <typename T, auto Value>
auto defaultValue() {
    using Scalar = typename Nullable<T>::Type;
    if constexpr (std::same_as<Scalar, std::string>) {
        constexpr auto text = Value.view();
        static_assert(text.find('\0') == std::string_view::npos, "SQL text defaults cannot contain NUL");
        return std::string(text);
    } else if constexpr (std::is_scoped_enum_v<Scalar>) {
        static_assert(std::same_as<decltype(Value), Scalar>, "enum default must match the field type");
        static_assert(!enumLabel(Value).empty());
        return Scalar(Value);
    } else if constexpr (std::same_as<Scalar, DaoDateTime>) {
        constexpr auto datetime = parseDaoDateTime(Value.view());
        return datetime;
    } else if constexpr (std::same_as<Scalar, bool>) {
        static_assert(std::same_as<decltype(Value), bool>, "boolean defaults require true or false");
        return Value;
    } else if constexpr (std::signed_integral<Scalar>) {
        static_assert(std::signed_integral<decltype(Value)> && std::in_range<Scalar>(Value),
                      "integer default must fit the field");
        return static_cast<Scalar>(Value);
    } else {
        static_assert(std::same_as<Scalar, double> && std::same_as<decltype(Value), double>,
                      "real defaults require a double template argument");
        constexpr double number = Value;
        static_assert(number >= -std::numeric_limits<double>::max() &&
                      number <= std::numeric_limits<double>::max(), "default must be finite");
        return number;
    }
}


}

template <typename T, auto Value>
struct Default {
    T value = detail::defaultValue<T, Value>();

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
    static std::optional<ColumnDefault> defaultMetadata() { return std::nullopt; }
    static constexpr std::optional<ForeignKey> foreignKey() { return std::nullopt; }
};

template <typename T, auto Initial>
struct FieldTraits<Default<T, Initial>> : FieldTraits<T> {
    static T& unwrap(Default<T, Initial>& field) { return field.value; }
    static std::optional<ColumnDefault> defaultMetadata() { auto value = defaultValue<T, Initial>();
        if constexpr (std::is_scoped_enum_v<decltype(value)>) {
            return ColumnDefault{std::string(enumLabel(value))};
        } else if constexpr (std::signed_integral<decltype(value)>) {
            return ColumnDefault{static_cast<std::int64_t>(value)};
        } else {
            return ColumnDefault{std::move(value)};
        } }
};

template <typename T, ColumnConstraint Constraint>
struct FieldTraits<SchemaField<T, Constraint>> : FieldTraits<T> {
    using Inner = FieldTraits<T>;
    static constexpr bool primary = Inner::primary || Constraint == ColumnConstraint::primary;
    static constexpr bool unique = Inner::unique || Constraint == ColumnConstraint::unique;
    static constexpr bool indexed = Inner::indexed || Constraint == ColumnConstraint::indexed;
    static auto& unwrap(SchemaField<T, Constraint>& field) { return Inner::unwrap(field.value); }
    static void setKey(SchemaField<T, Constraint>& field, typename Inner::Value key) {
        Inner::setKey(field.value, std::move(key));
    }
};

template <typename T>
constexpr ColumnType columnType() {
    if constexpr (std::same_as<T, std::string> || std::is_scoped_enum_v<T>) return ColumnType::text;
    else if constexpr (std::same_as<T, bool>) return ColumnType::boolean;
    else if constexpr (std::signed_integral<T>) return ColumnType::integer;
    else if constexpr (std::same_as<T, double>) return ColumnType::real;
    else if constexpr (std::same_as<T, DaoDateTime>) return ColumnType::datetime;
    else static_assert(!sizeof(T), "unsupported database field type");
}

}

}
