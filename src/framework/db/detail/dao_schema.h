#pragma once

// Included by base_dao.h after the BaseDao declaration.

namespace APP_NAMESPACE {

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::finish_initialization(Derived dao) -> drogon::Task<Derived> {
    auto& base = static_cast<BaseDao&>(dao);
    co_await base.database_->initialize_table(base.table_name_, std::move(base.schema_), composite_uniqueness());
    co_return std::move(dao);
}

template <typename Derived, typename Record, DaoOptions options>
BaseDao<Derived, Record, options>::BaseDao(DatabaseBackendPtr database)
    : database_(std::move(database)), table_name_(Fields::table_name), table_(detail::quote_identifier(table_name_)) {
    if (!database_) throw std::invalid_argument("DAO requires a database backend");
    build_schema(indices_);
    std::string projection, columns, placeholders, assignments, key_parameter;
    std::size_t insert_index = 0, update_index = 0;
    std::size_t flag_index = names_.size() + Record::generated_id;
    for (const auto& field : schema_) {
        const auto column = detail::quote_identifier(field.name);
        if (!projection.empty()) projection += ", ";
        projection += column;
        if (field.nullable) optional_columns_.push_back(field.name);
        else append_column(required_projection_, column);
        if (field.timestamp || field.name == "deleted_at") continue;

        const auto marker = parameter(++update_index);
        if (field.primary) {
            key_ = column;
            key_parameter = marker;
        } else {
            if (!assignments.empty()) assignments += ", ";
            assignments += column + " = ";
            if (field.nullable) {
                assignments += "CASE WHEN " + parameter(++flag_index) + " THEN " + marker +
                               " ELSE " + column + " END";
            } else {
                assignments += marker;
            }
        }
        if (!field.generated) {
            if (!columns.empty()) {
                columns += ", ";
                placeholders += ", ";
            }
            columns += column;
            placeholders += parameter(++insert_index);
        }
    }
    if (assignments.empty()) assignments = key_ + " = " + key_;
    update_ = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ + " = " + key_parameter + " AND deleted_at IS NULL";
    select_ = "SELECT " + projection + " FROM " + table_;
    insert_ = "INSERT INTO " + table_;
    if (columns.empty()) insert_ += " DEFAULT VALUES";
    else insert_ += " (" + columns + ") VALUES (" + placeholders + ")";
}

template <typename Derived, typename Record, DaoOptions options>
template <auto... members>
auto BaseDao<Derived, Record, options>::add_composite_unique(CompositeUniqueness& constraints, CompositeUnique<members...>) -> void {
    static_assert((std::same_as<Fields, typename detail::DaoMember<decltype(members)>::Record> && ...),
                  "CompositeUnique members must belong to the DAO fields type");
    constraints.push_back({std::string(glz::get_name<members>())...});
}

template <typename Derived, typename Record, DaoOptions options>
template <typename... Constraints>
auto BaseDao<Derived, Record, options>::add_composite_unique(CompositeUniqueness& result, const std::tuple<Constraints...>& constraints) -> void {
    std::apply([&](const auto&... constraint) { (add_composite_unique(result, constraint), ...); }, constraints);
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::composite_uniqueness() -> CompositeUniqueness {
    CompositeUniqueness result;
    if constexpr (requires { Fields::constraints; }) {
        add_composite_unique(result, Fields::constraints);
    }
    return result;
}

template <typename Derived, typename Record, DaoOptions options>
template <std::size_t index>
auto BaseDao<Derived, Record, options>::add_field() -> void {
    constexpr auto name = names_[index];
    static_assert(name != "deleted_at" && !timestamp_name(name) && (!Record::generated_id || name != "id"),
                  "reserved DAO fields are inherited");
    static_assert(!Traits<index>::foreign_key() || !Traits<index>::primary, "foreign fields cannot be primary keys");
    auto field = DatabaseColumn{std::string(name), detail::column_type<typename Value<index>::Type>(),
                       Value<index>::nullable, Traits<index>::primary, Traits<index>::unique, Traits<index>::indexed,
                       Traits<index>::default_metadata()};
    field.foreign_key = Traits<index>::foreign_key();
    using Scalar = typename Value<index>::Type;
    if constexpr (std::is_scoped_enum_v<Scalar>) {
        constexpr auto labels = magic_enum::enum_names<Scalar>();
        static_assert(!labels.empty(), "DAO enum has no reflectable values");
        field.enum_labels.assign(labels.begin(), labels.end());
        std::ranges::sort(field.enum_labels);
    }
    if (field.foreign_key && field.default_value) {
        throw std::invalid_argument("foreign fields cannot declare a default");
    }
    schema_.push_back(std::move(field));
}

template <typename Derived, typename Record, DaoOptions options>
template <std::size_t... index>
auto BaseDao<Derived, Record, options>::build_schema(std::index_sequence<index...>) -> void {
    (add_field<index>(), ...);
    if constexpr (Record::generated_id) {
        schema_.push_back({"id", ColumnType::integer, false, true, false, false, {}, true});
    }
    schema_.push_back({"deleted_at", ColumnType::datetime, true, false, false, false, {}});
    if constexpr (options.timestamps) {
        for (const auto name : timestamp_names_) {
            DatabaseColumn field{std::string(name), ColumnType::datetime, true, false, false, true, {}, false, true};
            field.index_descending = true;
            schema_.push_back(std::move(field));
        }
    }
}

}
