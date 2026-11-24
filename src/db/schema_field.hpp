#pragma once

#include "app_namespace.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace APP_NAMESPACE {

enum class ColumnType { text, integer, boolean, real };

using ColumnDefault = std::variant<std::string, std::int64_t, bool, double>;

struct DatabaseColumn {
    std::string name;
    ColumnType type;
    bool nullable, primary, unique, indexed;
    std::optional<ColumnDefault> defaultValue;
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

struct SqlReal {
    double value;
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

template <typename T, auto Value>
auto defaultValue() {
    using Scalar = typename Nullable<T>::Type;
    if constexpr (std::same_as<Scalar, std::string>) {
        constexpr auto text = Value.view();
        static_assert(text.find('\0') == std::string_view::npos, "SQL text defaults cannot contain NUL");
        return std::string(text);
    } else if constexpr (std::same_as<Scalar, bool>) {
        static_assert(std::same_as<decltype(Value), bool>, "boolean defaults require true or false");
        return Value;
    } else if constexpr (std::signed_integral<Scalar>) {
        static_assert(std::signed_integral<decltype(Value)> && std::in_range<Scalar>(Value),
                      "integer default must fit the field");
        return static_cast<Scalar>(Value);
    } else {
        static_assert(std::same_as<Scalar, double> && std::same_as<decltype(Value), SqlReal>,
                      "real defaults require SqlReal");
        constexpr double number = Value.value;
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
};

template <typename T, auto Initial>
struct FieldTraits<Default<T, Initial>> : FieldTraits<T> {
    static T& unwrap(Default<T, Initial>& field) { return field.value; }
    static std::optional<ColumnDefault> defaultMetadata() { auto value = defaultValue<T, Initial>();
        if constexpr (std::signed_integral<decltype(value)>) {
            return ColumnDefault{static_cast<std::int64_t>(value)};
        } else {
            return ColumnDefault{std::move(value)};
        } }
};

template <typename T, ColumnConstraint Constraint>
struct FieldTraits<SchemaField<T, Constraint>> : FieldTraits<T> {
    using Inner = FieldTraits<T>;
    static constexpr bool primary = Constraint == ColumnConstraint::primary;
    static constexpr bool unique = Constraint == ColumnConstraint::unique;
    static constexpr bool indexed = Constraint == ColumnConstraint::indexed;
    static auto& unwrap(SchemaField<T, Constraint>& field) { return Inner::unwrap(field.value); }
};

template <typename T>
constexpr ColumnType columnType() {
    if constexpr (std::same_as<T, std::string>) return ColumnType::text;
    else if constexpr (std::same_as<T, bool>) return ColumnType::boolean;
    else if constexpr (std::signed_integral<T>) return ColumnType::integer;
    else if constexpr (std::same_as<T, double>) return ColumnType::real;
    else static_assert(!sizeof(T), "unsupported database field type");
}

}

}
