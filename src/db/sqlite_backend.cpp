#include "database_backend.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <stdexcept>

namespace APP_NAMESPACE {

namespace detail {
std::string quoteIdentifier(std::string_view identifier) {
    std::string quoted = "\"";
    for (const char character : identifier) {
        if (character == '"') quoted += '"';
        quoted += character;
    }
    return quoted + '"';
}


}

namespace {

using detail::quoteIdentifier;

template <typename T>
std::string sqlLiteral(const T& value) {
    if constexpr (std::same_as<T, std::string>) {
        std::string literal = "'";
        for (char character : value) {
            if (character == '\'') literal += '\'';
            literal += character;
        }
        return literal + '\'';
    } else if constexpr (std::same_as<T, DaoDateTime>) {
        return sqlLiteral(detail::formatDaoDateTime(value));
    } else if constexpr (std::same_as<T, double>) {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc{}) throw std::runtime_error("could not format SQLite real default");
        return {buffer, result.ptr};
    } else {
        return std::to_string(value);
    }
}

std::string_view sqliteType(ColumnType type) {
    switch (type) {
        case ColumnType::text:
        case ColumnType::datetime: return "TEXT";
        case ColumnType::integer:
        case ColumnType::boolean: return "INTEGER";
        case ColumnType::real: return "REAL";
    }
    throw std::invalid_argument("unsupported SQLite column type");
}

std::optional<std::string> sqliteDefault(const DatabaseColumn& field) {
    if (!field.defaultValue) return std::nullopt;
    return std::visit([](const auto& value) { return sqlLiteral(value); }, *field.defaultValue);
}

std::string normalizeSchemaSql(std::string_view sql) {
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
        : client_(client), tableName_(std::move(table)), table_(quoteIdentifier(tableName_)), schema_(std::move(schema)),
          composite_(std::move(composite)) {}

    drogon::Task<> apply() const {
        std::string definitions;
        for (const auto& field : schema_) {
            if (!definitions.empty()) definitions += ", ";
            definitions += columnDefinition(field);
        }

        co_await executeSql("CREATE TABLE IF NOT EXISTS " + table_ + " (" + definitions + ")");
        co_await reconcileSchema(true);
        co_await validateGeneratedKey();
        co_await createIndexes();
        co_await createCompositeIndexes();
        co_await createTimestampTriggers();
        co_await reconcileSchema(false);
        co_await validateForeignKeys();
        co_await validateEnumConstraints();
    }

private:
    template <typename... Args>
    drogon::Task<drogon::orm::Result> executeSql(std::string sql, Args... args) const {
        return detail::executeOn(client_, std::move(sql), std::move(args)...);
    }

    drogon::Task<std::string> tableDefinition() const {
        const auto rows = co_await executeSql("SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = ?", tableName_);
        if (rows.empty()) schemaError("missing table definition");
        co_return rows[0][0].as<std::string>();
    }

    drogon::Task<> validateGeneratedKey() const {
        if (std::none_of(schema_.begin(), schema_.end(), [](const auto& field) { return field.generated; })) {
            co_return;
        }
        const auto definition = co_await tableDefinition();
        if (!normalizeSchemaSql(definition).contains("IDINTEGERPRIMARYKEYAUTOINCREMENT")) {
            schemaError("id must use AUTOINCREMENT");
        }
    }

    drogon::Task<> createTimestampTriggers() const {
        const auto hasTimestamps = std::any_of(schema_.begin(), schema_.end(),
                                               [](const auto& field) { return field.timestamp; });
        if (!hasTimestamps) co_return;
        std::string key, updateColumns;
        for (const auto& field : schema_) {
            if (field.timestamp) continue;
            if (!updateColumns.empty()) updateColumns += ", ";
            updateColumns += quoteIdentifier(field.name);
            if (field.primary) key = quoteIdentifier(field.name);
        }
        const auto prefix = "app_ts_" + tableName_ + "_";
        const auto stamp = "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
        for (const bool insert : {true, false}) {
            const auto name = prefix + (insert ? "insert" : "update");
            const auto existing = co_await executeSql(
                "SELECT tbl_name FROM sqlite_schema WHERE type = 'trigger' AND name = ?", name);
            if (!existing.empty() && existing[0][0].as<std::string>() != tableName_) schemaError("invalid trigger " + name);
            co_await executeSql("DROP TRIGGER IF EXISTS " + quoteIdentifier(name));
            const auto event = insert ? std::string("INSERT") : "UPDATE OF " + updateColumns;
            const auto assignments = (insert ? std::string("created_at = ") + stamp + ", " : "") +
                                     "updated_at = " + stamp;
            co_await executeSql("CREATE TRIGGER " + quoteIdentifier(name) + " AFTER " + event +
                " ON " + table_ + " BEGIN UPDATE " + table_ + " SET " + assignments +
                " WHERE " + key + " = NEW." + key + "; END");
        }
    }

    drogon::Task<> createIndexes() const {
        for (const auto& column : schema_) {
            if (!column.indexed || column.primary || column.unique) continue;
            const auto index = "idx_" + std::to_string(tableName_.size()) + "_" + tableName_ + "_" + column.name;
            co_await executeSql("CREATE INDEX IF NOT EXISTS " + quoteIdentifier(index) +
                                " ON " + table_ + " (" + quoteIdentifier(column.name) +
                                (column.indexDescending ? " DESC)" : ")"));
            const auto indexes = co_await executeSql("SELECT [unique], partial FROM pragma_index_list(?) WHERE name = ?", tableName_, index);
            const auto fields = co_await executeSql("SELECT name, [desc] FROM pragma_index_xinfo(?) WHERE [key] = 1", index);
            if (indexes.size() != 1 || indexes[0][0].template as<int>() != 0 ||
                indexes[0][1].template as<int>() != 0 || fields.size() != 1 ||
                fields[0][0].template as<std::string>() != column.name ||
                fields[0][1].template as<int>() != column.indexDescending) {
                schemaError("invalid index " + index);
            }
        }
    }

    drogon::Task<bool> hasCompositeIndex(const std::vector<std::string>& columns) const {
        const auto indexes = co_await executeSql(
            "SELECT name FROM pragma_index_list(?) WHERE [unique] = 1 AND partial = 0", tableName_);
        for (const auto& index : indexes) {
            const auto keys = co_await executeSql(
                "SELECT name, coll FROM pragma_index_xinfo(?) WHERE [key] = 1 ORDER BY seqno",
                index[0].as<std::string>());
            if (keys.size() != columns.size()) continue;

            bool matches = true;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (keys[i][0].isNull() || keys[i][0].as<std::string>() != columns[i] ||
                    keys[i][1].as<std::string>() != "BINARY") {
                    matches = false;
                    break;
                }
            }
            if (matches) co_return true;
        }
        co_return false;
    }

    drogon::Task<> createCompositeIndexes() const {
        for (const auto& columns : composite_) {
            if (co_await hasCompositeIndex(columns)) continue;
            std::string index = "uq_" + std::to_string(tableName_.size()) + "_" + tableName_;
            std::string keys;
            for (const auto& column : columns) {
                index += "_" + std::to_string(column.size()) + "_" + column;
                if (!keys.empty()) keys += ", ";
                keys += quoteIdentifier(column);
            }
            co_await executeSql("CREATE UNIQUE INDEX " + quoteIdentifier(index) +
                                " ON " + table_ + " (" + keys + ")");
        }
    }

    [[noreturn]] void schemaError(const std::string& detail) const {
        throw std::runtime_error("Schema mismatch for " + tableName_ + ": " + detail + "; explicit migration required");
    }

    static std::string enumConstraintName(const DatabaseColumn& field) {
        return "CONSTRAINT " + quoteIdentifier("dao_enum_" + field.name) + " ";
    }

    static std::string enumConstraint(const DatabaseColumn& field) {
        auto check = enumConstraintName(field) + "CHECK (" + quoteIdentifier(field.name) + " IN (";
        for (std::size_t i = 0; i < field.enumLabels.size(); ++i) {
            if (i != 0) check += ", ";
            check += sqlLiteral(std::string(field.enumLabels[i]));
        }
        return check + "))";
    }

    drogon::Task<> validateEnumConstraints() const {
        const auto definition = co_await tableDefinition();
        for (const auto& field : schema_) {
            if (field.enumLabels.empty() ? definition.contains(enumConstraintName(field))
                                         : !definition.contains(enumConstraint(field))) {
                schemaError("enum constraint for " + field.name);
            }
        }
    }

    static std::string columnDefinition(const DatabaseColumn& field) {
        auto definition = quoteIdentifier(field.name) + " " + std::string(sqliteType(field.type));
        if (field.primary) definition += " PRIMARY KEY";
        if (field.generated) definition += " AUTOINCREMENT";
        if (!field.nullable) definition += " NOT NULL";
        if (field.unique && !field.primary) definition += " UNIQUE";
        if (const auto value = sqliteDefault(field)) definition += " DEFAULT " + *value;
        if (const auto target = field.foreignKey) {
            definition += " REFERENCES " + quoteIdentifier(target->table) + " (" +
                          quoteIdentifier(target->column) + ") ON UPDATE RESTRICT ON DELETE RESTRICT";
        }
        if (!field.enumLabels.empty()) definition += " " + enumConstraint(field);
        return definition;
    }

    drogon::Task<> validateForeignKeys() const {
        const auto constraints = co_await executeSql(
            "SELECT [from], [table], [to], on_update, on_delete, seq FROM pragma_foreign_key_list(?)",
            tableName_);
        std::size_t expected = 0;
        for (const auto& field : schema_) {
            if (!field.foreignKey) continue;
            ++expected;
            const auto target = *field.foreignKey;
            const auto parent = co_await executeSql(
                "SELECT name, upper(type) FROM pragma_table_info(?) WHERE pk > 0",
                std::string(target.table));
            if (parent.size() != 1 || parent[0][0].as<std::string>() != target.column ||
                parent[0][1].as<std::string>() != sqliteType(field.type)) {
                schemaError("invalid foreign key target for " + field.name);
            }

            const auto count = std::count_if(constraints.begin(), constraints.end(), [&](const auto& row) {
                return row[0].template as<std::string>() == field.name &&
                       row[1].template as<std::string>() == target.table &&
                       row[2].template as<std::string>() == target.column &&
                       row[3].template as<std::string>() == "RESTRICT" &&
                       row[4].template as<std::string>() == "RESTRICT" &&
                       row[5].template as<int>() == 0;
            });
            if (count != 1) schemaError("foreign key for " + field.name);
        }
        if (constraints.size() != expected) schemaError("unexpected foreign key constraint");
        if (expected != 0) {
            const auto violations = co_await executeSql("SELECT 1 FROM pragma_foreign_key_check(?) LIMIT 1", tableName_);
            if (!violations.empty()) schemaError("orphaned foreign key rows");
        }
    }

    drogon::Task<> reconcileSchema(bool addMissing) const {
        const auto columns = co_await executeSql(
            "SELECT name, upper(type), [notnull], pk, dflt_value FROM pragma_table_info(?)", tableName_);
        for (const auto& column : columns) {
            const auto name = column[0].template as<std::string>();
            if (std::none_of(schema_.begin(), schema_.end(), [&](const DatabaseColumn& field) { return field.name == name; })) {
                schemaError("extra column " + name);
            }
        }
        const auto uniqueColumns = co_await executeSql(
            "SELECT info.name FROM pragma_index_list(?) AS idx "
            "JOIN pragma_index_info(idx.name) AS info "
            "WHERE idx.[unique] = 1 AND idx.origin != 'pk' AND idx.partial = 0 "
            "AND (SELECT count(*) FROM pragma_index_info(idx.name)) = 1", tableName_);
        std::vector<const DatabaseColumn*> missing;
        for (const auto& expected : schema_) {
            auto actual = std::find_if(columns.begin(), columns.end(), [&](const auto& row) {
                return row[0].template as<std::string>() == expected.name;
            });
            if (actual == columns.end()) {
                if (!addMissing || expected.primary || expected.unique ||
                    (!expected.nullable && !expected.defaultValue)) {
                    schemaError("missing column " + expected.name);
                }
                missing.push_back(&expected);
                continue;
            }
            const bool unique = std::any_of(uniqueColumns.begin(), uniqueColumns.end(), [&](const auto& row) {
                return !row[0].isNull() && row[0].template as<std::string>() == expected.name;
            });
            const auto defaultField = (*actual)[4];
            std::optional<std::string> actualDefault;
            if (!defaultField.isNull()) actualDefault = defaultField.template as<std::string>();
            if ((*actual)[1].template as<std::string>() != sqliteType(expected.type) ||
                (*actual)[2].template as<int>() != !expected.nullable ||
                (*actual)[3].template as<int>() != expected.primary || unique != (expected.unique && !expected.primary) ||
                actualDefault != sqliteDefault(expected)) {
                schemaError("column " + expected.name);
            }
        }
        for (const auto* field : missing) {
            co_await executeSql("ALTER TABLE " + table_ + " ADD COLUMN " + columnDefinition(*field));
        }
    }

    const drogon::orm::DbClientPtr& client_;
    std::string tableName_, table_;
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
        if (!usable_.load()) throw std::runtime_error("SQLite backend unavailable after failed rollback");
        return client_;
    }
    std::string_view currentTimestampSql() const override {
        return "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
    }
    std::string parameter(std::size_t index) const override { return "?" + std::to_string(index); }
    std::string containsText(std::string_view column, std::size_t index) const override {
        return "instr(" + std::string(column) + ", " + parameter(index) + ") > 0";
    }
    std::string nullSafeEqual(std::string_view column, std::size_t index) const override {
        return std::string(column) + " IS " + parameter(index);
    }
    std::optional<std::reference_wrapper<drogon::Mutex>> queryGate() const override { return std::ref(gate_); }

    drogon::Task<> transaction(TransactionCallback work) override {
        const auto lock = co_await gate_.scoped_lock();
        const auto& connection = client();
        co_await detail::executeOn(connection, "BEGIN IMMEDIATE");

        SqliteTransaction tx(*this);
        std::exception_ptr failure;
        try {
            co_await work(tx);
            co_await detail::executeOn(connection, "COMMIT");
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            std::exception_ptr rollbackFailure;
            try {
                co_await detail::executeOn(connection, "ROLLBACK");
            } catch (...) {
                usable_.store(false);
                rollbackFailure = std::current_exception();
            }
            if (rollbackFailure) reportRollbackFailure(failure, rollbackFailure);
            std::rethrow_exception(failure);
        }
    }

    drogon::Task<> initializeTable(
        std::string table, std::vector<DatabaseColumn> columns, CompositeUniqueness composite) override {
        co_await transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            const SqliteSchema schema(tx.client(), std::move(table), std::move(columns), std::move(composite));
            co_await schema.apply();
        });
    }

private:
    [[noreturn]] static void reportRollbackFailure(std::exception_ptr original, std::exception_ptr rollback) {
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

drogon::Task<DatabaseBackendPtr> openSqliteDatabase(std::filesystem::path path) {
    std::string connection = "filename='";
    for (char character : path.string()) {
        if (character == '\\' || character == '\'') connection += '\\';
        connection += character;
    }
    connection += '\'';
    auto client = drogon::orm::DbClient::newSqlite3Client(connection, 1);
    client->setTimeout(5.0);
    co_await detail::executeOn(client, "PRAGMA busy_timeout = 3000");
    co_await detail::executeOn(client, "PRAGMA foreign_keys = ON");
    const auto enforcement = co_await detail::executeOn(client, "PRAGMA foreign_keys");
    if (enforcement.empty() || enforcement[0][0].as<int>() != 1) {
        throw std::runtime_error("could not enable SQLite foreign key enforcement");
    }
    co_return std::make_shared<SqliteBackend>(std::move(client));
}

}
