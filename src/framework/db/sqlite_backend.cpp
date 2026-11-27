#include "database_backend.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <stdexcept>

namespace APP_NAMESPACE {

namespace detail {
std::string quote_identifier(std::string_view identifier) {
    std::string quoted = "\"";
    for (const char character : identifier) {
        if (character == '"')
            quoted += '"';
        quoted += character;
    }
    return quoted + '"';
}

}

namespace {

using detail::quote_identifier;

template <typename T>
std::string sql_literal(const T& value) {
    if constexpr (std::same_as<T, std::string>) {
        std::string literal = "'";
        for (char character : value) {
            if (character == '\'')
                literal += '\'';
            literal += character;
        }
        return literal + '\'';
    } else if constexpr (std::same_as<T, DaoDateTime>) {
        return sql_literal(detail::format_dao_date_time(value));
    } else if constexpr (std::same_as<T, double>) {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc{})
            throw std::runtime_error("could not format SQLite real default");
        return {buffer, result.ptr};
    } else {
        return std::to_string(value);
    }
}

std::string_view sqlite_type(ColumnType type) {
    switch (type) {
    case ColumnType::text:
    case ColumnType::datetime:
        return "TEXT";
    case ColumnType::integer:
    case ColumnType::boolean:
        return "INTEGER";
    case ColumnType::real:
        return "REAL";
    }
    throw std::invalid_argument("unsupported SQLite column type");
}

std::optional<std::string> sqlite_default(const DatabaseColumn& field) {
    if (!field.default_value)
        return std::nullopt;
    return std::visit([](const auto& value) { return sql_literal(value); }, *field.default_value);
}

std::string normalize_schema_sql(std::string_view sql) {
    std::string normalized;
    for (const unsigned char character : sql) {
        if (character != '"' && !std::isspace(character)) {
            normalized += static_cast<char>(std::toupper(character));
        }
    }
    return normalized;
}

class SqliteSchema {
  public:
    SqliteSchema(const drogon::orm::DbClientPtr& client, std::string table, std::vector<DatabaseColumn> schema,
                 CompositeUniqueness composite)
        : client_(client), table_name_(std::move(table)), table_(quote_identifier(table_name_)), schema_(std::move(schema)),
          composite_(std::move(composite)) {}

    drogon::Task<> apply() const {
        std::string definitions;
        for (const auto& field : schema_) {
            if (!definitions.empty())
                definitions += ", ";
            definitions += column_definition(field);
        }

        co_await execute_sql("CREATE TABLE IF NOT EXISTS " + table_ + " (" + definitions + ")");
        co_await reconcile_schema(true);
        co_await validate_generated_key();
        co_await create_indexes();
        co_await create_composite_indexes();
        co_await create_timestamp_triggers();
        co_await reconcile_schema(false);
        co_await validate_foreign_keys();
        co_await validate_enum_constraints();
    }

  private:
    template <typename... Args>
    drogon::Task<drogon::orm::Result> execute_sql(std::string sql, Args... args) const {
        return detail::execute_on(client_, std::move(sql), std::move(args)...);
    }

    drogon::Task<std::string> table_definition() const {
        const auto rows = co_await execute_sql("SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = ?", table_name_);
        if (rows.empty())
            schema_error("missing table definition");
        co_return rows[0][0].as<std::string>();
    }

    drogon::Task<> validate_generated_key() const {
        if (std::none_of(schema_.begin(), schema_.end(), [](const auto& field) { return field.generated; })) {
            co_return;
        }
        const auto definition = co_await table_definition();
        if (!normalize_schema_sql(definition).contains("IDINTEGERPRIMARYKEYAUTOINCREMENT")) {
            schema_error("id must use AUTOINCREMENT");
        }
    }

    drogon::Task<> create_timestamp_triggers() const {
        const auto has_timestamps = std::any_of(schema_.begin(), schema_.end(),
                                                [](const auto& field) { return field.timestamp; });
        if (!has_timestamps)
            co_return;
        std::string key, update_columns;
        for (const auto& field : schema_) {
            if (field.timestamp)
                continue;
            if (!update_columns.empty())
                update_columns += ", ";
            update_columns += quote_identifier(field.name);
            if (field.primary)
                key = quote_identifier(field.name);
        }
        const auto prefix = "app_ts_" + table_name_ + "_";
        const auto stamp = "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
        for (const bool insert : {true, false}) {
            const auto name = prefix + (insert ? "insert" : "update");
            const auto existing = co_await execute_sql(
                "SELECT tbl_name FROM sqlite_schema WHERE type = 'trigger' AND name = ?", name);
            if (!existing.empty() && existing[0][0].as<std::string>() != table_name_)
                schema_error("invalid trigger " + name);
            co_await execute_sql("DROP TRIGGER IF EXISTS " + quote_identifier(name));
            const auto event = insert ? std::string("INSERT") : "UPDATE OF " + update_columns;
            const auto assignments = (insert ? std::string("created_at = ") + stamp + ", " : "") +
                                     "updated_at = " + stamp;
            co_await execute_sql("CREATE TRIGGER " + quote_identifier(name) + " AFTER " + event +
                                 " ON " + table_ + " BEGIN UPDATE " + table_ + " SET " + assignments +
                                 " WHERE " + key + " = NEW." + key + "; END");
        }
    }

    drogon::Task<> create_indexes() const {
        for (const auto& column : schema_) {
            if (!column.indexed || column.primary || column.unique)
                continue;
            const auto index = "idx_" + std::to_string(table_name_.size()) + "_" + table_name_ + "_" + column.name;
            co_await execute_sql("CREATE INDEX IF NOT EXISTS " + quote_identifier(index) +
                                 " ON " + table_ + " (" + quote_identifier(column.name) +
                                 (column.index_descending ? " DESC)" : ")"));
            const auto indexes = co_await execute_sql("SELECT [unique], partial FROM pragma_index_list(?) WHERE name = ?", table_name_, index);
            const auto fields = co_await execute_sql("SELECT name, [desc] FROM pragma_index_xinfo(?) WHERE [key] = 1", index);
            if (indexes.size() != 1 || indexes[0][0].template as<int>() != 0 ||
                indexes[0][1].template as<int>() != 0 || fields.size() != 1 ||
                fields[0][0].template as<std::string>() != column.name ||
                fields[0][1].template as<int>() != column.index_descending) {
                schema_error("invalid index " + index);
            }
        }
    }

    drogon::Task<bool> has_composite_index(const std::vector<std::string>& columns) const {
        const auto indexes = co_await execute_sql(
            "SELECT name FROM pragma_index_list(?) WHERE [unique] = 1 AND partial = 0", table_name_);
        for (const auto& index : indexes) {
            const auto keys = co_await execute_sql(
                "SELECT name, coll FROM pragma_index_xinfo(?) WHERE [key] = 1 ORDER BY seqno",
                index[0].as<std::string>());
            if (keys.size() != columns.size())
                continue;

            bool matches = true;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (keys[i][0].isNull() || keys[i][0].as<std::string>() != columns[i] ||
                    keys[i][1].as<std::string>() != "BINARY") {
                    matches = false;
                    break;
                }
            }
            if (matches)
                co_return true;
        }
        co_return false;
    }

    drogon::Task<> create_composite_indexes() const {
        for (const auto& columns : composite_) {
            if (co_await has_composite_index(columns))
                continue;
            std::string index = "uq_" + std::to_string(table_name_.size()) + "_" + table_name_;
            std::string keys;
            for (const auto& column : columns) {
                index += "_" + std::to_string(column.size()) + "_" + column;
                if (!keys.empty())
                    keys += ", ";
                keys += quote_identifier(column);
            }
            co_await execute_sql("CREATE UNIQUE INDEX " + quote_identifier(index) +
                                 " ON " + table_ + " (" + keys + ")");
        }
    }

    [[noreturn]] void schema_error(const std::string& detail) const {
        throw std::runtime_error("Schema mismatch for " + table_name_ + ": " + detail + "; explicit migration required");
    }

    static std::string enum_constraint_name(const DatabaseColumn& field) {
        return "CONSTRAINT " + quote_identifier("dao_enum_" + field.name) + " ";
    }

    static std::string enum_constraint(const DatabaseColumn& field) {
        auto check = enum_constraint_name(field) + "CHECK (" + quote_identifier(field.name) + " IN (";
        for (std::size_t i = 0; i < field.enum_labels.size(); ++i) {
            if (i != 0)
                check += ", ";
            check += sql_literal(std::string(field.enum_labels[i]));
        }
        return check + "))";
    }

    drogon::Task<> validate_enum_constraints() const {
        const auto definition = co_await table_definition();
        for (const auto& field : schema_) {
            if (field.enum_labels.empty() ? definition.contains(enum_constraint_name(field))
                                          : !definition.contains(enum_constraint(field))) {
                schema_error("enum constraint for " + field.name);
            }
        }
    }

    static std::string column_definition(const DatabaseColumn& field) {
        auto definition = quote_identifier(field.name) + " " + std::string(sqlite_type(field.type));
        if (field.primary)
            definition += " PRIMARY KEY";
        if (field.generated)
            definition += " AUTOINCREMENT";
        if (!field.nullable)
            definition += " NOT NULL";
        if (field.unique && !field.primary)
            definition += " UNIQUE";
        if (const auto value = sqlite_default(field))
            definition += " DEFAULT " + *value;
        if (const auto target = field.foreign_key) {
            definition += " REFERENCES " + quote_identifier(target->table) + " (" +
                          quote_identifier(target->column) + ") ON UPDATE RESTRICT ON DELETE RESTRICT";
        }
        if (!field.enum_labels.empty())
            definition += " " + enum_constraint(field);
        return definition;
    }

    drogon::Task<> validate_foreign_keys() const {
        const auto constraints = co_await execute_sql(
            "SELECT [from], [table], [to], on_update, on_delete, seq FROM pragma_foreign_key_list(?)",
            table_name_);
        std::size_t expected = 0;
        for (const auto& field : schema_) {
            if (!field.foreign_key)
                continue;
            ++expected;
            const auto target = *field.foreign_key;
            const auto parent = co_await execute_sql(
                "SELECT name, upper(type) FROM pragma_table_info(?) WHERE pk > 0",
                std::string(target.table));
            if (parent.size() != 1 || parent[0][0].as<std::string>() != target.column ||
                parent[0][1].as<std::string>() != sqlite_type(field.type)) {
                schema_error("invalid foreign key target for " + field.name);
            }

            const auto count = std::count_if(constraints.begin(), constraints.end(), [&](const auto& row) {
                return row[0].template as<std::string>() == field.name &&
                       row[1].template as<std::string>() == target.table &&
                       row[2].template as<std::string>() == target.column &&
                       row[3].template as<std::string>() == "RESTRICT" &&
                       row[4].template as<std::string>() == "RESTRICT" &&
                       row[5].template as<int>() == 0;
            });
            if (count != 1)
                schema_error("foreign key for " + field.name);
        }
        if (constraints.size() != expected)
            schema_error("unexpected foreign key constraint");
        if (expected != 0) {
            const auto violations = co_await execute_sql("SELECT 1 FROM pragma_foreign_key_check(?) LIMIT 1", table_name_);
            if (!violations.empty())
                schema_error("orphaned foreign key rows");
        }
    }

    drogon::Task<> reconcile_schema(bool add_missing) const {
        const auto columns = co_await execute_sql(
            "SELECT name, upper(type), [notnull], pk, dflt_value FROM pragma_table_info(?)", table_name_);
        for (const auto& column : columns) {
            const auto name = column[0].template as<std::string>();
            if (std::none_of(schema_.begin(), schema_.end(), [&](const DatabaseColumn& field) { return field.name == name; })) {
                schema_error("extra column " + name);
            }
        }
        const auto unique_columns = co_await execute_sql(
            "SELECT info.name FROM pragma_index_list(?) AS idx "
            "JOIN pragma_index_info(idx.name) AS info "
            "WHERE idx.[unique] = 1 AND idx.origin != 'pk' AND idx.partial = 0 "
            "AND (SELECT count(*) FROM pragma_index_info(idx.name)) = 1",
            table_name_);
        std::vector<const DatabaseColumn*> missing;
        for (const auto& expected : schema_) {
            auto actual = std::find_if(columns.begin(), columns.end(), [&](const auto& row) {
                return row[0].template as<std::string>() == expected.name;
            });
            if (actual == columns.end()) {
                if (!add_missing || expected.primary || expected.unique ||
                    (!expected.nullable && !expected.default_value)) {
                    schema_error("missing column " + expected.name);
                }
                missing.push_back(&expected);
                continue;
            }
            const bool unique = std::any_of(unique_columns.begin(), unique_columns.end(), [&](const auto& row) {
                return !row[0].isNull() && row[0].template as<std::string>() == expected.name;
            });
            const auto default_field = (*actual)[4];
            std::optional<std::string> actual_default;
            if (!default_field.isNull())
                actual_default = default_field.template as<std::string>();
            if ((*actual)[1].template as<std::string>() != sqlite_type(expected.type) ||
                (*actual)[2].template as<int>() != !expected.nullable ||
                (*actual)[3].template as<int>() != expected.primary || unique != (expected.unique && !expected.primary) ||
                actual_default != sqlite_default(expected)) {
                schema_error("column " + expected.name);
            }
        }
        for (const auto* field : missing) {
            co_await execute_sql("ALTER TABLE " + table_ + " ADD COLUMN " + column_definition(*field));
        }
    }

    const drogon::orm::DbClientPtr& client_;
    std::string table_name_, table_;
    std::vector<DatabaseColumn> schema_;
    CompositeUniqueness composite_;
};

class SqliteTransaction final : public DaoTransaction {
  public:
    explicit SqliteTransaction(const DatabaseBackend& owner) : owner_(owner) {}
    const DatabaseBackend& backend() const override { return owner_; }
    const drogon::orm::DbClientPtr& client() const override { return owner_.client(); }

  private:
    const DatabaseBackend& owner_;
};

class SqliteBackend final : public DatabaseBackend {
  public:
    explicit SqliteBackend(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

    const drogon::orm::DbClientPtr& client() const override {
        if (!usable_.load())
            throw std::runtime_error("SQLite backend unavailable after failed rollback");
        return client_;
    }
    std::string_view current_timestamp_sql() const override {
        return "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
    }
    std::string parameter(std::size_t index) const override { return "?" + std::to_string(index); }
    std::string contains_text(std::string_view column, std::size_t index) const override {
        return "instr(" + std::string(column) + ", " + parameter(index) + ") > 0";
    }
    std::string null_safe_equal(std::string_view column, std::size_t index) const override {
        return std::string(column) + " IS " + parameter(index);
    }
    std::optional<std::reference_wrapper<drogon::Mutex>> query_gate() const override { return std::ref(gate_); }

    drogon::Task<> transaction(TransactionCallback work) override {
        const auto lock = co_await gate_.scoped_lock();
        const auto& connection = client();
        co_await detail::execute_on(connection, "BEGIN IMMEDIATE");

        SqliteTransaction tx(*this);
        std::exception_ptr failure;
        try {
            co_await work(tx);
            co_await detail::execute_on(client(), "COMMIT");
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            std::exception_ptr rollback_failure;
            try {
                co_await detail::execute_on(connection, "ROLLBACK");
            } catch (...) {
                usable_.store(false);
                rollback_failure = std::current_exception();
            }
            if (rollback_failure)
                report_rollback_failure(failure, rollback_failure);
            std::rethrow_exception(failure);
        }
    }

    drogon::Task<> savepoint(const DaoTransaction& tx, BatchCallback work) override {
        if (std::addressof(tx.backend()) != this) {
            throw std::invalid_argument("DAO and transaction must use the same database backend");
        }
        const auto& connection = tx.client();
        co_await detail::execute_on(connection, "SAVEPOINT app_dao_batch");
        std::exception_ptr failure;
        try {
            co_await work(tx);
            co_await detail::execute_on(connection, "RELEASE SAVEPOINT app_dao_batch");
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            std::exception_ptr rollback_failure;
            try {
                co_await detail::execute_on(connection, "ROLLBACK TO SAVEPOINT app_dao_batch");
                co_await detail::execute_on(connection, "RELEASE SAVEPOINT app_dao_batch");
            } catch (...) {
                usable_.store(false);
                rollback_failure = std::current_exception();
            }
            if (rollback_failure)
                report_rollback_failure(failure, rollback_failure);
            std::rethrow_exception(failure);
        }
    }

    drogon::Task<> initialize_table(
        std::string table, std::vector<DatabaseColumn> columns, CompositeUniqueness composite) override {
        co_await transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            const SqliteSchema schema(tx.client(), std::move(table), std::move(columns), std::move(composite));
            co_await schema.apply();
        });
    }

  private:
    [[noreturn]] static void report_rollback_failure(std::exception_ptr original, std::exception_ptr rollback) {
        std::string message = "SQLite rollback failed";
        try {
            std::rethrow_exception(rollback);
        } catch (const std::exception& error) {
            message += ": ";
            message += error.what();
        } catch (...) {
        }
        try {
            std::rethrow_exception(original);
        } catch (...) {
            std::throw_with_nested(std::runtime_error(message));
        }
    }

    std::atomic<bool> usable_{true};
    drogon::orm::DbClientPtr client_;
    mutable drogon::Mutex gate_;
};

}

drogon::Task<DatabaseBackendPtr> open_sqlite_database(std::filesystem::path path) {
    std::string connection = "filename='";
    for (char character : path.string()) {
        if (character == '\\' || character == '\'')
            connection += '\\';
        connection += character;
    }
    connection += '\'';
    auto client = drogon::orm::DbClient::newSqlite3Client(connection, 1);
    client->setTimeout(5.0);
    co_await detail::execute_on(client, "PRAGMA busy_timeout = 3000");
    co_await detail::execute_on(client, "PRAGMA foreign_keys = ON");
    const auto enforcement = co_await detail::execute_on(client, "PRAGMA foreign_keys");
    if (enforcement.empty() || enforcement[0][0].as<int>() != 1) {
        throw std::runtime_error("could not enable SQLite foreign key enforcement");
    }
    co_return std::make_shared<SqliteBackend>(std::move(client));
}

}
