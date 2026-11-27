#pragma once

#include "schema_field.h"

#include <glaze/reflection/get_name.hpp>

#include <initializer_list>
#include <ranges>

namespace APP_NAMESPACE {

namespace detail {

template <typename MemberPointer>
struct DaoMember;

template <typename Owner, typename Value>
struct DaoMember<Value Owner::*> {
    using Record = Owner;
    using Type = typename FieldTraits<std::remove_cv_t<Value>>::Value;
};

enum class PredicateOp { eq,
                         gt,
                         gte,
                         lt,
                         lte,
                         in,
                         contains };

template <PredicateOp op, typename T>
struct FilterOperand {
    T value;
};

template <auto member>
struct DaoPredicate {
    using Record = typename DaoMember<decltype(member)>::Record;
    using Value = typename DaoMember<decltype(member)>::Type;
    static constexpr auto name = glz::get_name<member>();

    PredicateOp operation;
    std::vector<Value> values;
    bool negated = false;
};

template <auto first_member, auto second_member>
struct DaoTupleLess {
    using FirstValue = typename Nullable<typename DaoMember<decltype(first_member)>::Type>::Type;
    using SecondValue = typename Nullable<typename DaoMember<decltype(second_member)>::Type>::Type;

    FirstValue first;
    SecondValue second;
};

template <PredicateOp op, typename T>
auto operand(T&& value) {
    if constexpr (std::is_convertible_v<T, std::string_view> &&
                  !std::same_as<std::decay_t<T>, std::nullptr_t>) {
        return FilterOperand<op, std::string>{std::string(std::forward<T>(value))};
    } else {
        return FilterOperand<op, std::decay_t<T>>{std::forward<T>(value)};
    }
}

template <typename Value, PredicateOp op, typename T>
constexpr bool valid_operand() {
    using Scalar = typename Nullable<Value>::Type;
    if constexpr (op == PredicateOp::contains) {
        return std::same_as<Scalar, std::string> && std::constructible_from<std::string, T>;
    } else if constexpr (op == PredicateOp::in) {
        return std::constructible_from<Value, typename T::value_type>;
    } else if constexpr (op == PredicateOp::eq) {
        return std::constructible_from<Value, T>;
    } else {
        return (std::signed_integral<Scalar> || std::same_as<Scalar, double> ||
                std::same_as<Scalar, DaoDateTime>) &&
               std::constructible_from<Scalar, T>;
    }
}

}

template <typename T>
auto eq(T&& value) { return detail::operand<detail::PredicateOp::eq>(std::forward<T>(value)); }
template <typename T>
auto gt(T&& value) { return detail::operand<detail::PredicateOp::gt>(std::forward<T>(value)); }
template <typename T>
auto gte(T&& value) { return detail::operand<detail::PredicateOp::gte>(std::forward<T>(value)); }
template <typename T>
auto lt(T&& value) { return detail::operand<detail::PredicateOp::lt>(std::forward<T>(value)); }
template <typename T>
auto lte(T&& value) { return detail::operand<detail::PredicateOp::lte>(std::forward<T>(value)); }

template <auto first_member, auto second_member, typename FirstValue, typename SecondValue>
    requires std::is_member_object_pointer_v<decltype(first_member)> &&
             std::is_member_object_pointer_v<decltype(second_member)> &&
             (detail::valid_operand<typename detail::DaoMember<decltype(first_member)>::Type,
                                    detail::PredicateOp::lt, FirstValue>()) &&
             (detail::valid_operand<typename detail::DaoMember<decltype(second_member)>::Type,
                                    detail::PredicateOp::lt, SecondValue>())
auto where_tuple_less(FirstValue&& first, SecondValue&& second) {
    using Predicate = detail::DaoTupleLess<first_member, second_member>;
    return Predicate{typename Predicate::FirstValue(std::forward<FirstValue>(first)),
                     typename Predicate::SecondValue(std::forward<SecondValue>(second))};
}

inline auto contains(std::string_view value) {
    return detail::operand<detail::PredicateOp::contains>(value);
}

template <std::ranges::input_range Range>
auto in(Range&& values) {
    using Value = std::ranges::range_value_t<Range>;
    using Owned = decltype(detail::operand<detail::PredicateOp::in>(std::declval<Value>()).value);
    std::vector<Owned> owned;
    for (auto&& value : values) {
        owned.push_back(detail::operand<detail::PredicateOp::in>(Value(value)).value);
    }
    return detail::FilterOperand<detail::PredicateOp::in, std::vector<Owned>>{std::move(owned)};
}

template <typename T>
auto in(std::initializer_list<T> values) {
    return in(std::ranges::subrange(values.begin(), values.end()));
}

template <auto member, detail::PredicateOp op, typename T>
    requires std::is_member_object_pointer_v<decltype(member)> &&
             (detail::valid_operand<typename detail::DaoMember<decltype(member)>::Type, op, T>())
auto where(detail::FilterOperand<op, T> operand) {
    using Predicate = detail::DaoPredicate<member>;
    using Value = typename Predicate::Value;
    Predicate predicate{op, {}};
    if constexpr (op == detail::PredicateOp::in) {
        for (const auto& value : operand.value)
            predicate.values.emplace_back(value);
    } else if constexpr (op == detail::PredicateOp::eq || op == detail::PredicateOp::contains) {
        predicate.values.emplace_back(std::move(operand.value));
    } else {
        using Scalar = typename detail::Nullable<Value>::Type;
        predicate.values.emplace_back(Scalar(std::move(operand.value)));
    }
    return predicate;
}

template <auto member, typename Value>
    requires std::is_member_object_pointer_v<decltype(member)> &&
             std::constructible_from<typename detail::DaoMember<decltype(member)>::Type, Value>
auto where(Value&& value) {
    return where<member>(eq(std::forward<Value>(value)));
}

template <auto member>
auto negate(detail::DaoPredicate<member> predicate) {
    predicate.negated = !predicate.negated;
    return predicate;
}

}
