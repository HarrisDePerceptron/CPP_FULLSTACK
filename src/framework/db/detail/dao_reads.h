#pragma once

// Included by base_dao.h after the BaseDao declaration.

namespace APP_NAMESPACE {

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::read_by_id(TransactionScope tx, Id id, bool include) const -> drogon::Task<std::optional<Record>> {
    const auto rows = co_await execute(tx, select_ + " WHERE " + key_ + " = " + parameter(1) + (include ? "" : " AND deleted_at IS NULL"), own_argument(std::move(id)));
    if (rows.empty())
        co_return std::nullopt;
    co_return map_row(rows[0]);
}

template <typename Derived, typename Record, DaoOptions options>
template <auto... members, typename... TargetDaos>
auto BaseDao<Derived, Record, options>::read_with_foreign(
    TransactionScope tx, Id id, bool include, detail::ForeignSelection<members, TargetDaos>... selections) const -> drogon::Task<std::optional<Record>> {
    if (!(selections.dao.get().uses_backend(database_) && ...)) {
        throw std::invalid_argument("foreign DAOs must use the same database backend");
    }
    auto record = co_await read_by_id(tx, std::move(id), include);
    if (record) {
        if (tx) {
            ((co_await selections.dao.get().load_foreign(tx->get(), detail::foreign_field((*record).*members))), ...);
        } else {
            ((co_await selections.dao.get().load_foreign(detail::foreign_field((*record).*members))), ...);
        }
    }
    co_return record;
}

template <typename Derived, typename Record, DaoOptions options>
template <typename... Predicates>
auto BaseDao<Derived, Record, options>::count_records(TransactionScope tx, bool include, Predicates... predicates) const -> drogon::Task<std::int64_t> {
    std::string sql = "SELECT COUNT(*) FROM " + table_;
    std::vector<detail::QueryValue> values;
    append_where(sql, values, include, predicates...);
    const auto rows = co_await execute_bound(tx, std::move(sql), std::move(values));
    co_return rows[0][0].template as<std::int64_t>();
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::map_row(const drogon::orm::Row& row, std::vector<std::string> omitted) const -> Record {
    if constexpr (!options.timestamps) {
        for (const auto name : timestamp_names_)
            omitted.emplace_back(name);
    }
    auto fields = row_to_struct<Fields>(row, omitted);
    Record result(std::move(fields), std::move(omitted));
    if (result.is_column_loaded("deleted_at")) {
        detail::read_row_field(result.deleted_at, row.at("deleted_at"), "deleted_at");
    }
    if constexpr (Record::generated_id)
        result.id = row.at("id").template as<Id>();
    if constexpr (options.timestamps) {
        if (result.is_column_loaded("created_at")) {
            detail::read_row_field(result.created_at, row.at("created_at"), "created_at");
        }
        if (result.is_column_loaded("updated_at")) {
            detail::read_row_field(result.updated_at, row.at("updated_at"), "updated_at");
        }
    }
    return result;
}

template <typename Derived, typename Record, DaoOptions options>
constexpr auto BaseDao<Derived, Record, options>::timestamp_name(std::string_view name) -> bool {
    return std::ranges::find(timestamp_names_, name) != timestamp_names_.end();
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::require_enabled_column(std::string_view name) -> void {
    if (!options.timestamps && timestamp_name(name)) {
        throw std::invalid_argument("timestamp queries require timestamps to be enabled");
    }
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::collect(FindQuery& query, EqualityFilters& filters) -> auto {
    auto& entries = query.equality.entries_;
    entries.insert(entries.end(), std::make_move_iterator(filters.entries_.begin()),
                   std::make_move_iterator(filters.entries_.end()));
    return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::collect(FindQuery& query, detail::IncludeDeleted) -> auto {
    query.include_deleted = true;
    return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::collect(FindQuery& query, detail::SelectAll) -> auto {
    query.all = true;
    return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
template <auto... members>
auto BaseDao<Derived, Record, options>::collect(FindQuery& query, detail::SelectFields<members...>) -> auto {
    ((require_enabled_column(glz::get_name<members>()),
      query.selected.push_back(glz::get_name<members>())),
     ...);
    return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
template <auto member>
auto BaseDao<Derived, Record, options>::collect(FindQuery& query, detail::OrderBy<member> order) -> auto {
    constexpr auto name = glz::get_name<member>();
    require_enabled_column(name);
    query.ordering.emplace_back(name, order.direction);
    return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
template <auto member>
auto BaseDao<Derived, Record, options>::collect(FindQuery&, detail::DaoPredicate<member>& predicate) -> auto {
    require_enabled_column(predicate.name);
    return std::tie(predicate);
}

template <typename Derived, typename Record, DaoOptions options>
template <auto first, auto second>
auto BaseDao<Derived, Record, options>::collect(FindQuery&, detail::DaoTupleLess<first, second>& predicate) -> auto {
    return std::tie(predicate);
}

template <typename Derived, typename Record, DaoOptions options>
template <auto member, typename Dao>
auto BaseDao<Derived, Record, options>::collect(FindQuery& query, detail::ForeignSelection<member, Dao>) -> auto {
    query.selected.push_back(glz::get_name<member>());
    return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::read_foreign_batch(
    TransactionScope tx, const std::set<Id>& keys) const -> drogon::Task<std::map<Id, Record>> {
    constexpr std::size_t batch_size = 500;
    const std::string key_name(Record::key_column);
    std::map<Id, Record> records;
    auto next = keys.begin();
    while (next != keys.end()) {
        std::string placeholders;
        std::vector<detail::QueryValue> values;
        while (next != keys.end() && values.size() < batch_size) {
            values.push_back(query_value(*next++));
            append_column(placeholders, parameter(values.size()));
        }
        auto rows = co_await execute_bound(tx, select_ + " WHERE " + key_ + " IN (" + placeholders + ") AND deleted_at IS NULL", std::move(values));
        for (const auto& row : rows) {
            auto key = detail::read_scalar<Id>(row.at(key_name));
            records.emplace(std::move(key), map_row(row));
        }
    }
    co_return records;
}

template <typename Derived, typename Record, DaoOptions options>
template <auto member, typename Dao>
auto BaseDao<Derived, Record, options>::load_foreign_page(TransactionScope tx, std::vector<Record>& records,
                                                          detail::ForeignSelection<member, Dao> selection) -> drogon::Task<> {
    using TargetId = typename Dao::RecordType::Id;
    std::set<TargetId> keys;
    for (auto& record : records) {
        const std::optional<TargetId> key(detail::foreign_field(record.*member).key());
        if (key)
            keys.insert(*key);
    }
    if (keys.empty())
        co_return;
    const auto targets = co_await selection.dao.get().read_foreign_batch(tx, keys);
    for (auto& record : records) {
        auto& field = detail::foreign_field(record.*member);
        const std::optional<TargetId> key(field.key());
        if (!key)
            continue;
        const auto target = targets.find(*key);
        if (target != targets.end())
            field.set_record(target->second);
    }
}

template <typename Derived, typename Record, DaoOptions options>
template <typename... Modifiers>
auto BaseDao<Derived, Record, options>::find_page(
    TransactionScope tx, std::int64_t limit, std::int64_t offset, Modifiers... modifiers) const -> drogon::Task<std::vector<Record>> {
    if (limit <= 0 || offset < 0)
        throw std::invalid_argument("invalid pagination");
    const auto foreign = std::tuple_cat(foreign_selection(modifiers)...);
    const auto same_backend = std::apply([&](const auto&... selections) {
        return (selections.dao.get().uses_backend(database_) && ...);
    },
                                         foreign);
    if (!same_backend)
        throw std::invalid_argument("foreign DAOs must use the same database backend");

    FindQuery query;
    // Collect left to right so explicit sort fields retain the caller's order.
    auto parts = std::tuple<decltype(collect(query, modifiers))...>{collect(query, modifiers)...};
    const auto predicates = std::apply([](auto&&... part) {
        return std::tuple_cat(std::move(part)...);
    },
                                       parts);

    std::string sql = select_;
    std::vector<std::string> omitted;
    if (!query.all) {
        auto projection = required_projection_;
        for (const auto& name : optional_columns_) {
            if (std::ranges::find(query.selected, name) == query.selected.end())
                omitted.push_back(name);
            else
                append_column(projection, detail::quote_identifier(name));
        }
        sql = "SELECT " + projection + " FROM " + table_;
    }
    if (query.ordering.empty() && options.timestamps) {
        query.ordering.emplace_back("updated_at", SortDirection::desc);
    }
    if (std::ranges::none_of(query.ordering, [](const auto& item) { return item.first == Record::key_column; })) {
        query.ordering.emplace_back(Record::key_column, SortDirection::asc);
    }
    std::string ordering;
    for (const auto& [name, direction] : query.ordering) {
        append_column(ordering, detail::quote_identifier(name) +
                                    (direction == SortDirection::desc ? " DESC NULLS LAST" : " ASC NULLS LAST"));
    }
    std::vector<detail::QueryValue> values;
    std::apply([&](const auto&... predicates) { append_where(sql, values, query.include_deleted, predicates...); }, predicates);
    bool has_where = !query.include_deleted || std::tuple_size_v<decltype(predicates)> != 0;
    for (auto& [name, value] : query.equality.entries_) {
        require_enabled_column(name);
        sql += has_where ? " AND " : " WHERE ";
        has_where = true;
        values.push_back(std::move(value));
        sql += database_->null_safe_equal(detail::quote_identifier(name), values.size());
    }
    sql += " ORDER BY " + ordering + " LIMIT " + parameter(values.size() + 1) +
           " OFFSET " + parameter(values.size() + 2);
    values.emplace_back(limit);
    values.emplace_back(offset);
    const auto rows = co_await execute_bound(tx, std::move(sql), std::move(values));

    std::vector<Record> records;
    records.reserve(rows.size());
    for (const auto& row : rows)
        records.push_back(map_row(row, omitted));
    if constexpr (std::tuple_size_v<decltype(foreign)> > 0) {
        const auto load = [&](const auto&... selections) -> drogon::Task<> {
            (co_await load_foreign_page(tx, records, selections), ...);
        };
        co_await std::apply(load, foreign);
    }
    co_return records;
}

template <typename Derived, typename Record, DaoOptions options>
template <auto member>
auto BaseDao<Derived, Record, options>::predicate_sql(const detail::DaoPredicate<member>& predicate,
                                                      std::vector<detail::QueryValue>& values) const -> std::string {
    using detail::PredicateOp;
    require_enabled_column(predicate.name);
    const auto column = detail::quote_identifier(predicate.name);
    std::string expression;
    if (predicate.operation == PredicateOp::in) {
        std::string list;
        bool includes_null = false;
        for (const typename detail::DaoPredicate<member>::Value value : predicate.values) {
            auto argument = query_value(value);
            if (std::holds_alternative<std::nullptr_t>(argument)) {
                includes_null = true;
            } else {
                values.push_back(std::move(argument));
                append_column(list, parameter(values.size()));
            }
        }
        expression = list.empty() ? "FALSE" : column + " IN (" + list + ")";
        if (includes_null)
            expression += " OR " + column + " IS NULL";
    } else {
        values.push_back(query_value(static_cast<typename detail::DaoPredicate<member>::Value>(
            predicate.values.front())));
        const auto index = values.size();
        switch (predicate.operation) {
        case PredicateOp::eq:
            expression = database_->null_safe_equal(column, index);
            break;
        case PredicateOp::gt:
            expression = column + " > " + parameter(index);
            break;
        case PredicateOp::gte:
            expression = column + " >= " + parameter(index);
            break;
        case PredicateOp::lt:
            expression = column + " < " + parameter(index);
            break;
        case PredicateOp::lte:
            expression = column + " <= " + parameter(index);
            break;
        case PredicateOp::contains:
            expression = database_->contains_text(column, index);
            break;
        case PredicateOp::in:
            break;
        }
    }
    if (predicate.negated)
        return "NOT COALESCE((" + expression + "), FALSE)";
    return "(" + expression + ")";
}

template <typename Derived, typename Record, DaoOptions options>
template <auto first, auto second>
auto BaseDao<Derived, Record, options>::predicate_sql(const detail::DaoTupleLess<first, second>& predicate,
                                                      std::vector<detail::QueryValue>& values) const -> std::string {
    constexpr auto first_column = glz::get_name<first>();
    constexpr auto second_column = glz::get_name<second>();
    require_enabled_column(first_column);
    require_enabled_column(second_column);
    const auto first_parameter = parameter(values.size() + 1);
    const auto second_parameter = parameter(values.size() + 2);
    values.push_back(query_value(predicate.first));
    values.push_back(query_value(predicate.second));
    return "(" + detail::quote_identifier(first_column) + ", " + detail::quote_identifier(second_column) +
           ") < (" + first_parameter + ", " + second_parameter + ")";
}

template <typename Derived, typename Record, DaoOptions options>
template <typename... Predicates>
auto BaseDao<Derived, Record, options>::append_where(std::string& sql, std::vector<detail::QueryValue>& values, bool include,
                                                     const Predicates&... predicates) const -> void {
    if (!include)
        sql += " WHERE deleted_at IS NULL";
    if constexpr (sizeof...(Predicates) > 0) {
        auto separator = include ? " WHERE " : " AND ";
        auto append = [&](const auto& predicate) {
            sql += separator + predicate_sql(predicate, values);
            separator = " AND ";
        };
        (append(predicates), ...);
    }
}

}
