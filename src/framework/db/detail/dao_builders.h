#pragma once

// Included by base_dao.h after the BaseDao declaration.

namespace APP_NAMESPACE {

template <typename Derived, typename Record, DaoOptions options>
template <typename Builder>
class BaseDao<Derived, Record, options>::NamedFilters {
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

template <typename Derived, typename Record, DaoOptions options>
template <typename... Modifiers>
class BaseDao<Derived, Record, options>::FindBuilder : public NamedFilters<FindBuilder<Modifiers...>> {
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

template <typename Derived, typename Record, DaoOptions options>
template <typename... Selections>
class BaseDao<Derived, Record, options>::FindByIdBuilder {
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

template <typename Derived, typename Record, DaoOptions options>
class BaseDao<Derived, Record, options>::UpdateByIdBuilder {
public:
    template <auto member, typename Value>
        requires writable_member<member> && std::constructible_from<FieldValue<member>, Value>
    UpdateByIdBuilder set(Value&& value) && {
        constexpr auto column = glz::get_name<member>();
        auto argument = query_value(FieldValue<member>(std::forward<Value>(value)));
        auto existing = std::ranges::find(assignments_, column, &PatchAssignment::column);
        if (existing == assignments_.end()) assignments_.push_back({column, std::move(argument)});
        else existing->value = std::move(argument);
        return std::move(*this);
    }

    drogon::Task<bool> execute() && {
        return dao_.patch_record(tx_, std::move(id_), std::move(assignments_));
    }

private:
    friend BaseDao;
    UpdateByIdBuilder(const BaseDao& dao, TransactionScope tx, Id id)
        : dao_(dao), tx_(tx), id_(std::move(id)) {}

    const BaseDao& dao_;
    TransactionScope tx_;
    Id id_;
    std::vector<PatchAssignment> assignments_;
};

template <typename Derived, typename Record, DaoOptions options>
template <typename... Predicates>
class BaseDao<Derived, Record, options>::CountBuilder : public NamedFilters<CountBuilder<Predicates...>> {
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

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::find() const & -> FindBuilder<> { return FindBuilder<>(*this, {}, 100, 0, {}); }

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::find(const DaoTransaction& tx) const & -> FindBuilder<> {
    return FindBuilder<>(*this, std::cref(tx), 100, 0, {});
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::find_by_id(Id id) const & -> FindByIdBuilder<> {
    return FindByIdBuilder<>(*this, {}, std::move(id), false, {});
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::find_by_id(const DaoTransaction& tx, Id id) const & -> FindByIdBuilder<> {
    return FindByIdBuilder<>(*this, std::cref(tx), std::move(id), false, {});
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::update_by_id(Id id) const & -> UpdateByIdBuilder {
    return UpdateByIdBuilder(*this, {}, std::move(id));
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::update_by_id(const DaoTransaction& tx, Id id) const & -> UpdateByIdBuilder {
    return UpdateByIdBuilder(*this, std::cref(tx), std::move(id));
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::count() const & -> CountBuilder<> { return CountBuilder<>(*this, {}, false, {}); }

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::count(const DaoTransaction& tx) const & -> CountBuilder<> {
    return CountBuilder<>(*this, std::cref(tx), false, {});
}

}
