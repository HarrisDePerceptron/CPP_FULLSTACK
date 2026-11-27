#pragma once

// Included by base_dao.h after the BaseDao declaration.

namespace APP_NAMESPACE {

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::create_record(TransactionScope tx,
                                                      Record record) const
    -> drogon::Task<Id> {
    check_id<true>(record);
    const auto result =
        co_await write_record<true>(tx, insert_ + " RETURNING " + key_, record);
    co_return detail::read_scalar<Id>(result[0][0]);
}

template <typename Derived, typename Record, DaoOptions options>
template <auto conflict_key>
auto BaseDao<Derived, Record, options>::conflict_target() -> std::string {
    return " ON CONFLICT (" +
           detail::quote_identifier(glz::get_name<conflict_key>()) + ")";
}

template <typename Derived, typename Record, DaoOptions options>
template <auto conflict_key>
auto BaseDao<Derived, Record, options>::insert_absent(TransactionScope tx,
                                                      Record record) const
    -> drogon::Task<bool> {
    check_id<true>(record);
    const auto result = co_await write_record<true>(
        tx, insert_ + conflict_target<conflict_key>() + " DO NOTHING", record);
    co_return result.affectedRows() != 0;
}

template <typename Derived, typename Record, DaoOptions options>
template <auto conflict_key, auto... members>
auto BaseDao<Derived, Record, options>::upsert_record(TransactionScope tx,
                                                      Record record) const
    -> drogon::Task<Id> {
    check_id<true>(record);
    std::string assignments;
    const auto add = [&](std::string_view name) {
        auto column = detail::quote_identifier(name);
        append_column(assignments, column + " = excluded." + column);
    };
    (add(glz::get_name<members>()), ...);
    const auto result = co_await write_record<true>(
        tx,
        insert_ + conflict_target<conflict_key>() + " DO UPDATE SET " +
            assignments + " RETURNING " + key_,
        record);
    co_return detail::read_scalar<Id>(result[0][0]);
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::patch_record(
    TransactionScope tx, Id id, std::vector<PatchAssignment> fields) const
    -> drogon::Task<bool> {
    if (fields.empty())
        throw std::invalid_argument("updateById requires at least one field");
    std::string assignments;
    std::vector<detail::QueryValue> values;
    values.reserve(fields.size() + 1);
    for (auto& field : fields) {
        values.push_back(std::move(field.value));
        append_column(assignments, detail::quote_identifier(field.column) +
                                       " = " + parameter(values.size()));
    }
    values.push_back(query_value(id));
    auto sql = "UPDATE " + table_ + " SET " + assignments + " WHERE " + key_ +
               " = " + parameter(values.size()) + " AND deleted_at IS NULL";
    const auto result =
        co_await execute_bound(tx, std::move(sql), std::move(values));
    co_return result.affectedRows() != 0;
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::update_record(TransactionScope tx,
                                                      Record record) const
    -> drogon::Task<bool> {
    check_id<false>(record);
    const auto result = co_await write_record<false>(tx, update_, record);
    co_return result.affectedRows() != 0;
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::write_batch(TransactionScope tx,
                                                    BatchCallback work) const
    -> drogon::Task<> {
    if (tx)
        return database_->savepoint(tx->get(), std::move(work));
    return database_->transaction(std::move(work));
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::create_batch(
    TransactionScope tx, std::vector<Record> records) const
    -> drogon::Task<std::vector<Id>> {
    std::vector<Id> ids;
    if (records.empty())
        co_return ids;
    ids.reserve(records.size());
    co_await write_batch(
        tx, [&](const DaoTransaction& batch_tx) -> drogon::Task<> {
            for (auto& record : records)
                ids.push_back(co_await create(batch_tx, std::move(record)));
        });
    co_return ids;
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::update_batch(
    TransactionScope tx, std::vector<Record> records) const
    -> drogon::Task<bool> {
    if (records.empty())
        co_return true;
    try {
        co_await write_batch(
            tx, [&](const DaoTransaction& batch_tx) -> drogon::Task<> {
                for (auto& record : records) {
                    if (!co_await update(batch_tx, std::move(record)))
                        throw UnmatchedBulkUpdate{};
                }
            });
    } catch (const UnmatchedBulkUpdate&) {
        co_return false;
    }
    co_return true;
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::delete_batch(TransactionScope tx,
                                                     std::vector<Id> ids,
                                                     bool soft) const
    -> drogon::Task<std::int64_t> {
    std::int64_t changed = 0;
    if (ids.empty())
        co_return changed;
    co_await write_batch(
        tx, [&](const DaoTransaction& batch_tx) -> drogon::Task<> {
            for (auto& id : ids) {
                const auto deleted =
                    soft ? co_await soft_delete_by_id(batch_tx, std::move(id))
                         : co_await delete_by_id(batch_tx, std::move(id));
                if (deleted)
                    ++changed;
            }
        });
    co_return changed;
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::delete_record(TransactionScope tx,
                                                      Id id) const
    -> drogon::Task<bool> {
    const auto result = co_await execute(
        tx, "DELETE FROM " + table_ + " WHERE " + key_ + " = " + parameter(1),
        own_argument(std::move(id)));
    co_return result.affectedRows() != 0;
}

template <typename Derived, typename Record, DaoOptions options>
auto BaseDao<Derived, Record, options>::set_deleted(TransactionScope tx, Id id,
                                                    bool deleted) const
    -> drogon::Task<bool> {
    const std::string value(deleted ? database_->current_timestamp_sql()
                                    : "NULL");
    const auto result =
        co_await execute(tx,
                         "UPDATE " + table_ + " SET deleted_at = " + value +
                             " WHERE " + key_ + " = " + parameter(1) +
                             (deleted ? " AND deleted_at IS NULL"
                                      : " AND deleted_at IS NOT NULL"),
                         own_argument(std::move(id)));
    co_return result.affectedRows() != 0;
}

template <typename Derived, typename Record, DaoOptions options>
template <bool insert>
auto BaseDao<Derived, Record, options>::check_id(const Record& record) -> void {
    if constexpr (Record::generated_id) {
        if (record.id.has_value() == insert) {
            throw std::invalid_argument(
                insert ? "create requires an empty generated ID"
                       : "update requires a generated ID");
        }
    }
}

template <typename Derived, typename Record, DaoOptions options>
template <std::size_t index, typename Field>
auto BaseDao<Derived, Record, options>::update_value(const Record& record,
                                                     Field& field)
    -> decltype(auto) {
    if constexpr (Value<index>::nullable) {
        using Argument = typename Traits<index>::Value;
        return record.is_column_loaded(names_[index])
                   ? Argument(Traits<index>::unwrap(field))
                   : Argument{};
    } else {
        return Traits<index>::unwrap(field);
    }
}

template <typename Derived, typename Record, DaoOptions options>
template <std::size_t index>
auto BaseDao<Derived, Record, options>::update_flag(const Record& record)
    -> auto {
    if constexpr (Value<index>::nullable)
        return std::tuple{record.is_column_loaded(names_[index])};
    else
        return std::tuple{};
}

template <typename Derived, typename Record, DaoOptions options>
template <bool insert>
auto BaseDao<Derived, Record, options>::write_record(TransactionScope tx,
                                                     const std::string& sql,
                                                     Record& record) const
    -> drogon::Task<drogon::orm::Result> {
    auto fields = glz::to_tie(static_cast<Fields&>(record));
    return [&]<std::size_t... index>(std::index_sequence<index...>) {
        if constexpr (insert) {
            return execute(tx, sql,
                           own_argument(Traits<index>::unwrap(
                               glz::get<index>(fields)))...);
        } else {
            return std::apply(
                [&](auto... flags) {
                    if constexpr (Record::generated_id) {
                        return execute(tx, sql,
                                       own_argument(update_value<index>(
                                           record, glz::get<index>(fields)))...,
                                       *record.id, flags...);
                    } else {
                        return execute(tx, sql,
                                       own_argument(update_value<index>(
                                           record, glz::get<index>(fields)))...,
                                       flags...);
                    }
                },
                std::tuple_cat(update_flag<index>(record)...));
        }
    }(indices_);
}

}
