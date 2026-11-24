#include "users.hpp"
#include "random.hpp"

#include <sqlite3.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace hello {
namespace {

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

[[noreturn]] void databaseError(sqlite3* db) {
    throw std::runtime_error(db ? sqlite3_errmsg(db) : "could not allocate SQLite connection");
}

Database open(const std::filesystem::path& path, int flags) {
    sqlite3* handle = nullptr;
    const auto status = sqlite3_open_v2(path.c_str(), &handle, flags, nullptr);
    Database db(handle, &sqlite3_close);
    if (status != SQLITE_OK || sqlite3_busy_timeout(handle, 3000) != SQLITE_OK) {
        databaseError(handle);
    }
    return db;
}

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) databaseError(db);
}

Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) databaseError(db);
    return Statement(stmt, &sqlite3_finalize);
}

void bindText(sqlite3_stmt* stmt, int index, std::string_view value) {
    if (sqlite3_bind_text64(stmt, index, value.data(), value.size(), SQLITE_TRANSIENT,
                            SQLITE_UTF8) != SQLITE_OK) {
        databaseError(sqlite3_db_handle(stmt));
    }
}

}

std::optional<Credentials> Users::seedDemo() const {
    auto db = open(database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    execute(db.get(), "CREATE TABLE IF NOT EXISTS users ("
                      "username TEXT PRIMARY KEY NOT NULL, password_hash TEXT NOT NULL, "
                      "seed_key TEXT UNIQUE)");
    execute(db.get(), "BEGIN IMMEDIATE");
    auto existing = prepare(db.get(), "SELECT 1 FROM users WHERE seed_key = 'demo'");
    const auto status = sqlite3_step(existing.get());
    existing.reset();
    if (status == SQLITE_ROW) {
        execute(db.get(), "COMMIT");
        return std::nullopt;
    }
    if (status != SQLITE_DONE) databaseError(db.get());

    Credentials credentials{"demo-" + randomHex<6>(), randomHex<16>()};
    std::array<char, crypto_pwhash_STRBYTES> hash;
    if (crypto_pwhash_str(hash.data(), credentials.password.data(), credentials.password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("could not hash demo user password");
    }
    auto insert = prepare(db.get(), "INSERT INTO users (username, password_hash, seed_key) "
                                    "VALUES (?, ?, 'demo')");
    bindText(insert.get(), 1, credentials.username);
    bindText(insert.get(), 2, hash.data());
    if (sqlite3_step(insert.get()) != SQLITE_DONE) databaseError(db.get());
    execute(db.get(), "COMMIT");
    return credentials;
}

bool Users::authenticate(std::string_view username, std::string_view password) const {
    auto db = open(database_, SQLITE_OPEN_READONLY);
    auto query = prepare(db.get(), "SELECT password_hash FROM users WHERE username = ?");
    bindText(query.get(), 1, username);
    const auto status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) return false;
    if (status != SQLITE_ROW) databaseError(db.get());

    const auto* hash = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0));
    if (!hash) databaseError(db.get());
    return crypto_pwhash_str_verify(hash, password.data(), password.size()) == 0;
}

}
