#include "database_backend.hpp"

#include <algorithm>
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
                 LegacyMigration migration = LegacyMigration::none)
        : client_(client), tableName_(std::move(table)), table_(quoteIdentifier(tableName_)), schema_(std::move(schema)),
          migration_(migration) {}

    drogon::Task<> apply() const {
        std::string definitions;
        for (const auto& field : schema_) {
            if (!definitions.empty()) definitions += ", ";
            definitions += columnDefinition(field);
        }

        co_await executeSql("BEGIN IMMEDIATE");
        std::exception_ptr failure;
        try {
            if (migration_ == LegacyMigration::usersUsernameKey) co_await migrateLegacyUsers(definitions);
            co_await executeSql("CREATE TABLE IF NOT EXISTS " + table_ + " (" + definitions + ")");
            if (migration_ == LegacyMigration::usersUsernameKey) {
                co_await renameColumn("passwordHash", "password_hash");
                co_await renameColumn("seedKey", "seed_key");
            }
            if (std::any_of(schema_.begin(), schema_.end(), [](const auto& field) { return field.timestamp; })) {
                co_await renameColumn("createdAt", "created_at");
                co_await renameColumn("updatedAt", "updated_at");
            }
            co_await reconcileSchema(true);
            co_await validateGeneratedKey();
            co_await createIndexes();
            co_await createTimestampTriggers();
            co_await reconcileSchema(false);
            co_await validateForeignKeys();
            co_await executeSql("COMMIT");
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            co_await executeSql("ROLLBACK");
            std::rethrow_exception(failure);
        }
    }

private:
    template <typename... Args>
    drogon::Task<drogon::orm::Result> executeSql(std::string sql, Args... args) const {
        return detail::executeOn(client_, std::move(sql), std::move(args)...);
    }

    drogon::Task<> migrateLegacyUsers(const std::string& definitions) const {
        const auto oldKey = co_await executeSql(
            "SELECT 1 FROM pragma_table_info(?) WHERE name = 'username' AND pk = 1", tableName_);
        if (oldKey.empty()) co_return;

        const SqliteSchema legacy(client_, tableName_, {
            {"username", ColumnType::text, false, true, false, false, {}},
            {"password_hash", ColumnType::text, false, false, false, false, {}},
            {"seed_key", ColumnType::text, true, false, true, false, {}},
        });
        co_await legacy.reconcileSchema(false);
        const auto definition = co_await tableDefinition();
        if (normalizeSchemaSql(definition) != normalizeSchemaSql(
            "CREATE TABLE users (username TEXT PRIMARY KEY NOT NULL, "
            "password_hash TEXT NOT NULL, seed_key TEXT UNIQUE)")) {
            schemaError("unexpected legacy users table definition");
        }

        const auto dependencies = co_await executeSql(
            "SELECT 1 FROM sqlite_schema WHERE (tbl_name = ?1 COLLATE NOCASE AND sql IS NOT NULL "
            "AND type IN ('index', 'trigger')) OR (type = 'view' AND instr(lower(sql), lower(?1)) > 0) "
            "UNION ALL SELECT 1 FROM sqlite_schema AS s JOIN pragma_foreign_key_list(s.name) AS fk "
            "WHERE s.type = 'table' AND (s.name = ?1 COLLATE NOCASE OR fk.[table] = ?1 COLLATE NOCASE)",
            tableName_);
        if (!dependencies.empty()) schemaError("legacy users table has dependent schema objects");

        const auto replacement = quoteIdentifier("__app_users_migration");
        co_await executeSql("CREATE TABLE " + replacement + " (" + definitions + ")");
        co_await executeSql("INSERT INTO " + replacement + " (username, password_hash, seed_key) "
                            "SELECT username, password_hash, seed_key FROM " + table_ + " ORDER BY username");
        co_await executeSql("DROP TABLE " + table_);
        co_await executeSql("ALTER TABLE " + replacement + " RENAME TO " + table_);
    }

    drogon::Task<> renameColumn(std::string oldName, std::string newName) const {
        const auto matches = co_await executeSql(
            "SELECT name FROM pragma_table_info(?1) WHERE name IN (?2, ?3)", tableName_, oldName, newName);
        if (matches.size() > 1) schemaError("both " + oldName + " and " + newName + " exist");
        if (matches.size() == 1 && matches[0][0].as<std::string>() == oldName) {
            co_await executeSql("ALTER TABLE " + table_ + " RENAME COLUMN " + quoteIdentifier(oldName) +
                                " TO " + quoteIdentifier(newName));
        }
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
            if (!column.indexed) continue;
            const auto index = "idx_" + std::to_string(tableName_.size()) + "_" + tableName_ + "_" + column.name;
            co_await executeSql("CREATE INDEX IF NOT EXISTS " + quoteIdentifier(index) +
                                " ON " + table_ + " (" + quoteIdentifier(column.name) + ")");
            const auto indexes = co_await executeSql("SELECT [unique], partial FROM pragma_index_list(?) WHERE name = ?", tableName_, index);
            const auto fields = co_await executeSql("SELECT name FROM pragma_index_info(?)", index);
            if (indexes.size() != 1 || indexes[0][0].template as<int>() != 0 ||
                indexes[0][1].template as<int>() != 0 || fields.size() != 1 ||
                fields[0][0].template as<std::string>() != column.name) {
                schemaError("invalid index " + index);
            }
        }
    }

    [[noreturn]] void schemaError(const std::string& detail) const {
        throw std::runtime_error("Schema mismatch for " + tableName_ + ": " + detail + "; explicit migration required");
    }

    static std::string columnDefinition(const DatabaseColumn& field) {
        auto definition = quoteIdentifier(field.name) + " " + std::string(sqliteType(field.type));
        if (field.primary) definition += " PRIMARY KEY";
        if (field.generated) definition += " AUTOINCREMENT";
        if (!field.nullable) definition += " NOT NULL";
        if (field.unique) definition += " UNIQUE";
        if (const auto value = sqliteDefault(field)) definition += " DEFAULT " + *value;
        if (const auto target = field.foreignKey) {
            definition += " REFERENCES " + quoteIdentifier(target->table) + " (" +
                          quoteIdentifier(target->column) + ") ON UPDATE RESTRICT ON DELETE RESTRICT";
        }
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
                (*actual)[3].template as<int>() != expected.primary || unique != expected.unique ||
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
    LegacyMigration migration_;
};

class SqliteBackend final : public DatabaseBackend {
public:
    explicit SqliteBackend(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

    const drogon::orm::DbClientPtr& client() const override { return client_; }
    std::string parameter(std::size_t index) const override { return "?" + std::to_string(index); }
    std::string nullSafeEqual(std::string_view column, std::size_t index) const override {
        return std::string(column) + " IS " + parameter(index);
    }
    std::optional<std::reference_wrapper<drogon::Mutex>> queryGate() const override { return std::ref(gate_); }

    // The single SQLite connection must be reserved for the entire schema transaction.
    drogon::Task<> initializeTable(std::string table, std::vector<DatabaseColumn> columns,
                                    LegacyMigration migration) override {
        const auto lock = co_await gate_.scoped_lock();
        const SqliteSchema schema(client_, std::move(table), std::move(columns), migration);
        co_await schema.apply();
    }

private:
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
