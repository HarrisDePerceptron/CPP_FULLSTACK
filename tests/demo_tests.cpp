#include "config.hpp"
#include "json_response.hpp"
#include "tokens.hpp"
#include "uploads.hpp"
#include "users.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace {
using namespace hello;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_((fs::temp_directory_path() / "hello-test-XXXXXX").string()) {
        if (!mkdtemp(path_.data())) throw std::runtime_error("could not create test directory");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    fs::path path() const { return path_; }

private:
    std::string path_;
};

class SavedEnvironment {
public:
    explicit SavedEnvironment(const char* name) : name_(name) {
        if (const auto* value = std::getenv(name)) old_ = value;
    }

    ~SavedEnvironment() {
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
    }

private:
    const char* name_;
    std::optional<std::string> old_;
};

TEST_CASE("Glaze reads and validates config.json", "[config]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "config.json";
    REQUIRE_THROWS(readConfig(path));
    std::ofstream(path) << R"({"port":8080,"jwt_lifetime_seconds":1800,"log_level":"warn"})";
    const auto config = readConfig(path);
    CHECK(config.port == 8080);
    CHECK(config.jwtLifetime == 1800s);
    CHECK(config.logLevel == LogLevel::warn);

    for (const auto invalid : {
             "not JSON",
             R"({"port":8080})",
             R"({"port":65536,"jwt_lifetime_seconds":1800,"log_level":"info"})",
             R"({"port":8080,"jwt_lifetime_seconds":0,"log_level":"info"})",
             R"({"port":8080,"jwt_lifetime_seconds":1800,"log_level":"invalid"})",
         }) {
        std::ofstream(path) << invalid;
        CHECK_THROWS(readConfig(path));
    }
}

TEST_CASE("An existing environment variable overrides .env", "[config]") {
    REQUIRE(sodium_init() >= 0);
    TemporaryDirectory temporary;
    SavedEnvironment saved("HELLO_JWT_SECRET");
    const auto path = temporary.path() / ".env";
    REQUIRE(unsetenv("HELLO_JWT_SECRET") == 0);

    CHECK(loadSigningKey(path).size() == 32);
    std::ofstream(path) << "HELLO_JWT_SECRET=" << std::string(64, 'a') << '\n';
    CHECK(loadSigningKey(path) == std::string(32, static_cast<char>(0xaa)));

    REQUIRE(setenv("HELLO_JWT_SECRET", std::string(64, '1').c_str(), 1) == 0);
    CHECK(loadSigningKey(path) == std::string(32, '\x11'));
    REQUIRE(setenv("HELLO_JWT_SECRET", "invalid", 1) == 0);
    CHECK_THROWS(loadSigningKey(path));
    REQUIRE(setenv("HELLO_JWT_SECRET", std::string(64, 'z').c_str(), 1) == 0);
    CHECK_THROWS(loadSigningKey(path));
}

TEST_CASE("SQLite persists only the hashed demo credential", "[users]") {
    REQUIRE(sodium_init() >= 0);
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "users.sqlite3";
    const Users firstRun(path);
    const auto credentials = firstRun.seedDemo();
    REQUIRE(credentials.has_value());
    CHECK(credentials->username.starts_with("demo-"));
    CHECK(credentials->password.size() == 32);

    const Users secondRun(path);
    CHECK_FALSE(secondRun.seedDemo());
    CHECK(secondRun.authenticate(credentials->username, credentials->password));
    CHECK_FALSE(secondRun.authenticate(credentials->username, "wrong"));
    CHECK_FALSE(secondRun.authenticate("missing", credentials->password));

    std::ifstream database(path, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(database), {}};
    CHECK(raw.find("$argon2id$") != std::string::npos);
    CHECK(raw.find(credentials->password) == std::string::npos);
}

TEST_CASE("Glaze JWT serialization and verification preserve signed claims", "[jwt]") {
    const auto key = std::string(32, 'x');
    const Tokens tokens(key, 1800s);
    const auto token = tokens.issue("demo-user");
    CHECK(tokens.verify(token) == std::optional<std::string>{"demo-user"});
    CHECK(Tokens(key, 1800s).verify(token) == std::optional<std::string>{"demo-user"});
    CHECK_FALSE(Tokens(std::string(32, 'y'), 1800s).verify(token));
    CHECK_FALSE(tokens.verify(""));
    CHECK_FALSE(tokens.verify("garbage"));
    auto modified = token;
    const auto signature = modified.rfind('.') + 1;
    modified[signature] = modified[signature] == 'A' ? 'B' : 'A';
    CHECK_FALSE(tokens.verify(modified));
    const Tokens expired(key, -1s);
    CHECK_FALSE(tokens.verify(expired.issue("demo-user")));
    CHECK_FALSE(tokens.verify(tokens.issue("")));
}

TEST_CASE("jsonResponse sets the status, content type and JSON body", "[http]") {
    const auto response = jsonError(drogon::k400BadRequest, "Invalid \"input\"");
    CHECK(response->statusCode() == drogon::k400BadRequest);
    CHECK(response->getContentType() == drogon::CT_APPLICATION_JSON);
    CHECK(response->body() == R"({"error":"Invalid \"input\""})");
}

TEST_CASE("Uploads are published under generated names with validated types", "[upload]") {
    REQUIRE(sodium_init() >= 0);
    TemporaryDirectory temporary;
    const Uploads uploads(temporary.path());
    for (const auto& [bytes, extension, mime] : {
             std::tuple{"%PDF-1.7\n%%EOF\n", ".pdf", "application/pdf"},
             std::tuple{"\x89PNG\r\n\x1a\n", ".png", "image/png"},
         }) {
        const auto saved = uploads.save(bytes);
        REQUIRE(saved.has_value());
        CHECK(saved->size() == 36);
        CHECK(saved->ends_with(extension));
        const auto found = uploads.find(*saved);
        REQUIRE(found.has_value());
        CHECK(found->second.mime == mime);
        std::ifstream file(found->first, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(file), {}};
        CHECK(contents == bytes);
    }
    CHECK(uploads.save("").error() == UploadError::empty);
    CHECK(uploads.save("<html></html>").error() == UploadError::unsupported);
    CHECK(uploads.save(std::string(maxUploadBytes + 1, 'x')).error() == UploadError::tooLarge);
    CHECK_FALSE(uploads.find("../users.sqlite3"));
    CHECK_FALSE(uploads.find(".pending-XVF9PQ"));
    const std::string symlink = std::string(32, 'f') + ".pdf";
    fs::create_symlink(temporary.path() / "target", temporary.path() / symlink);
    std::ofstream(temporary.path() / "target") << "%PDF-";
    CHECK_FALSE(uploads.find(symlink));
}
}
