#pragma once

#include "schema_field.hpp"

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

enum class PredicateOp { eq, gt, gte, lt, lte, in, contains };

template <PredicateOp Op, typename T>
struct FilterOperand {
    T value;
};

template <auto Member>
struct DaoPredicate {
    using Record = typename DaoMember<decltype(Member)>::Record;
    using Value = typename DaoMember<decltype(Member)>::Type;
    static constexpr auto name = glz::get_name<Member>();

    PredicateOp operation;
    std::vector<Value> values;
    bool negated = false;
};

template <PredicateOp Op, typename T>
auto operand(T&& value) {
    if constexpr (std::is_convertible_v<T, std::string_view> &&
                  !std::same_as<std::decay_t<T>, std::nullptr_t>) {
        return FilterOperand<Op, std::string>{std::string(std::forward<T>(value))};
    } else {
        return FilterOperand<Op, std::decay_t<T>>{std::forward<T>(value)};
    }
}

template <typename Value, PredicateOp Op, typename T>
constexpr bool validOperand() {
    using Scalar = typename Nullable<Value>::Type;
    if constexpr (Op == PredicateOp::contains) {
        return std::same_as<Scalar, std::string> && std::constructible_from<std::string, T>;
    } else if constexpr (Op == PredicateOp::in) {
        return std::constructible_from<Value, typename T::value_type>;
    } else if constexpr (Op == PredicateOp::eq) {
        return std::constructible_from<Value, T>;
    } else {
        return (std::signed_integral<Scalar> || std::same_as<Scalar, double> ||
                std::same_as<Scalar, DaoDateTime>) && std::constructible_from<Scalar, T>;
    }
}

}

template <typename T> auto eq(T&& value) { return detail::operand<detail::PredicateOp::eq>(std::forward<T>(value)); }
template <typename T> auto gt(T&& value) { return detail::operand<detail::PredicateOp::gt>(std::forward<T>(value)); }
template <typename T> auto gte(T&& value) { return detail::operand<detail::PredicateOp::gte>(std::forward<T>(value)); }
template <typename T> auto lt(T&& value) { return detail::operand<detail::PredicateOp::lt>(std::forward<T>(value)); }
template <typename T> auto lte(T&& value) { return detail::operand<detail::PredicateOp::lte>(std::forward<T>(value)); }

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

template <auto Member, detail::PredicateOp Op, typename T>
    requires std::is_member_object_pointer_v<decltype(Member)> &&
             (detail::validOperand<typename detail::DaoMember<decltype(Member)>::Type, Op, T>())
auto where(detail::FilterOperand<Op, T> operand) {
    using Predicate = detail::DaoPredicate<Member>;
    using Value = typename Predicate::Value;
    Predicate predicate{Op, {}};
    if constexpr (Op == detail::PredicateOp::in) {
        for (const auto& value : operand.value) predicate.values.emplace_back(value);
    } else if constexpr (Op == detail::PredicateOp::eq || Op == detail::PredicateOp::contains) {
        predicate.values.emplace_back(std::move(operand.value));
    } else {
        using Scalar = typename detail::Nullable<Value>::Type;
        predicate.values.emplace_back(Scalar(std::move(operand.value)));
    }
    return predicate;
}

template <auto Member, typename Value>
    requires std::is_member_object_pointer_v<decltype(Member)> &&
             std::constructible_from<typename detail::DaoMember<decltype(Member)>::Type, Value>
auto where(Value&& value) {
    return where<Member>(eq(std::forward<Value>(value)));
}

template <auto Member>
auto negate(detail::DaoPredicate<Member> predicate) {
    predicate.negated = !predicate.negated;
    return predicate;
}

}
