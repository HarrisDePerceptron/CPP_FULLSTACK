#include "app_namespace.hpp"

#include <glaze/chrono.hpp>
#include "db/row_to_struct.hpp"
#include "utils/config.hpp"
#include "server/json_response.hpp"
#include "server/auth/tokens.hpp"
#include "services/uploads.hpp"
#include "services/user_service.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <sodium.h>
#include <trantor/net/EventLoopThread.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace APP_NAMESPACE {

struct SampleRow {
    int count;
    std::optional<std::string> label;
};

struct IndexedFields {
    static constexpr std::string_view table_name = "indexed_records";

    Primary<int> id;
    Indexed<std::string> category;
    std::optional<std::string> note;
    Default<bool, false> active;
    Default<int, 5> priority;
    Default<double, 1.25> ratio;
    Default<DaoDateTime, SqlText{"2027-04-01T12:34:56.789Z"}> starts_at;
    std::optional<DaoDateTime> ends_at;
    Indexed<Default<std::string, SqlText{"O'Brien"}>> label;
};

using IndexedRecord = DaoRecord<IndexedFields>;

struct PostFields {
    static constexpr std::string_view table_name = "posts";

    std::string title;
    Indexed<RequiredForeign<UserRecord>> author_id;
    Foreign<IndexedRecord> reviewer_id;
};

using PostRecord = DaoRecord<PostFields>;

struct ComposedFields {
    static constexpr std::string_view table_name = "composed_records";

    Indexed<Unique<Default<std::string, SqlText{"first"}>>> label;
    Indexed<Unique<Foreign<UserRecord>>> owner_id;
};

using ComposedRecord = DaoRecord<ComposedFields>;

struct MembershipFields {
    static constexpr std::string_view table_name = "memberships";

    RequiredForeign<IndexedRecord> project_id;
    RequiredForeign<UserRecord> user_id;

    static constexpr auto constraints = CompositeUnique<
        &MembershipFields::project_id, &MembershipFields::user_id>{};
};

struct AssignmentFields {
    static constexpr std::string_view table_name = "assignments";

    std::string project;
    std::string person;
    std::string role;

    static constexpr auto constraints = std::tuple{
        CompositeUnique<&AssignmentFields::project, &AssignmentFields::person>{},
        CompositeUnique<&AssignmentFields::person, &AssignmentFields::role>{}
    };
};

enum class PostStatus { draft, published };
enum class ChangedStatus { draft, archived };

template <typename Status>
struct StatusFields {
    static constexpr std::string_view table_name = "status_records";

    std::string title;
    Default<Status, Status::draft> status;
    std::optional<Status> previous_status;
};


}


namespace {
using namespace APP_NAMESPACE;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_((fs::temp_directory_path() / "app-test-XXXXXX").string()) {
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

DatabaseBackendPtr openTestDatabase(const fs::path& path) {
    return drogon::sync_wait(openSqliteDatabase(path));
}

template <typename T>
T runOnEventLoop(drogon::Task<T> task) {
    trantor::EventLoopThread thread;
    thread.run();
    const auto run = [&]() -> drogon::Task<T> {
        co_await drogon::switchThreadCoro(thread.getLoop());
        co_return co_await task;
    };
    return drogon::sync_wait(run());
}

bool authenticate(const UserService& users, std::string username, std::string password) {
    Workers workers;
    return runOnEventLoop(users.authenticate(std::move(username), std::move(password), workers));
}

template <auto Member, typename Value>
concept SupportsWhere = requires(Value value) { where<Member>(value); };

template <typename Dao, typename Filter>
concept SupportsFind = requires(const Dao& dao, Filter filter) { dao.find(filter); };

static_assert(SupportsWhere<&UserRecord::username, std::string>);
static_assert(!SupportsWhere<&UserRecord::username, int>);
static_assert(!SupportsWhere<&UserDao::hasDemo, int>);
static_assert(!SupportsFind<UserDao, decltype(where<&SampleRow::count>(1))>);

template <typename Dao>
concept HasPublicInitialize = requires(Dao dao) { Dao::finishInitialization(std::move(dao)); };

static_assert(!std::constructible_from<UserDao, DatabaseBackendPtr>);
static_assert(!std::copy_constructible<UserDao>);
static_assert(!HasPublicInitialize<UserDao>);

class IndexedDao final : public BaseDao<IndexedDao, IndexedRecord> {
public:
    static drogon::Task<IndexedDao> init(DatabaseBackendPtr database) {
        co_return co_await finishInitialization(IndexedDao(std::move(database)));
    }

private:
    explicit IndexedDao(DatabaseBackendPtr database)
        : BaseDao(std::move(database)) {}
};

class UntimedUserDao final : public BaseDao<UntimedUserDao, UserRecord, DaoOptions{.timestamps = false}> {
public:
    static drogon::Task<UntimedUserDao> init(DatabaseBackendPtr database) {
        co_return co_await finishInitialization(UntimedUserDao(std::move(database)));
    }

private:
    explicit UntimedUserDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

class PostDao final : public BaseDao<PostDao, PostRecord> {
public:
    static drogon::Task<PostDao> init(DatabaseBackendPtr database) {
        co_return co_await finishInitialization(PostDao(std::move(database)));
    }

private:
    explicit PostDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

template <typename Fields>
class ConstrainedDao final : public BaseDao<ConstrainedDao<Fields>, DaoRecord<Fields>> {
    using Base = BaseDao<ConstrainedDao<Fields>, DaoRecord<Fields>>;

public:
    static drogon::Task<ConstrainedDao> init(DatabaseBackendPtr database) {
        return Base::finishInitialization(ConstrainedDao(std::move(database)));
    }

private:
    explicit ConstrainedDao(DatabaseBackendPtr database) : Base(std::move(database)) {}
};

template <auto... Members>
concept SupportsCompositeUnique = requires { typename CompositeUnique<Members...>; };

static_assert(!SupportsCompositeUnique<&MembershipFields::project_id>);
static_assert(!SupportsCompositeUnique<&MembershipFields::project_id, &MembershipFields::project_id>);
static_assert(!SupportsCompositeUnique<&MembershipFields::project_id, &UserRecord::username>);

TEST_CASE("Composite uniqueness prevents duplicate pivot pairs without replacing the generated ID", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    const auto projects = drogon::sync_wait(IndexedDao::init(database));
    const auto memberships = drogon::sync_wait(ConstrainedDao<MembershipFields>::init(database));
    const auto alice = drogon::sync_wait(users.create({{"alice", "hash"}}));
    const auto bob = drogon::sync_wait(users.create({{"bob", "hash"}}));
    drogon::sync_wait(projects.create({{7, "first"}}));
    IndexedRecord second{{8, "second"}};
    second.label.value.value = "different";
    drogon::sync_wait(projects.create(second));
    const auto id = drogon::sync_wait(memberships.create({{7, alice}}));
    CHECK(drogon::sync_wait(memberships.findById(id)));
    CHECK(drogon::sync_wait(memberships.create({{7, bob}})) > id);
    CHECK(drogon::sync_wait(memberships.create({{8, alice}})) > id);
    CHECK_THROWS(drogon::sync_wait(memberships.create({{7, alice}})));

    const auto assignments = drogon::sync_wait(ConstrainedDao<AssignmentFields>::init(database));
    drogon::sync_wait(assignments.create({{"website", "alice", "owner"}}));
    CHECK_THROWS(drogon::sync_wait(assignments.create({{"website", "alice", "editor"}})));
    CHECK_THROWS(drogon::sync_wait(assignments.create({{"docs", "alice", "owner"}})));
    CHECK(drogon::sync_wait(assignments.create({{"docs", "alice", "editor"}})) > 0);
}

TEST_CASE("Composite uniqueness rolls back on duplicates and reuses equivalent indexes", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto& client = database->client();
    client->execSqlSync("CREATE TABLE assignments (project TEXT NOT NULL, person TEXT NOT NULL, "
                        "role TEXT NOT NULL, id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL)");
    client->execSqlSync("INSERT INTO assignments (project, person, role) "
                        "VALUES ('docs', 'alice', 'editor'), ('build', 'alice', 'editor')");
    CHECK_THROWS(drogon::sync_wait(ConstrainedDao<AssignmentFields>::init(database)));
    CHECK(client->execSqlSync("SELECT COUNT(*) FROM assignments")[0][0].as<int>() == 2);
    CHECK(client->execSqlSync("SELECT name FROM pragma_table_info('assignments')").size() == 4);
    CHECK(client->execSqlSync("SELECT name FROM pragma_index_list('assignments')").empty());

    client->execSqlSync("DELETE FROM assignments WHERE project = 'build'");
    client->execSqlSync("CREATE UNIQUE INDEX existing_assignment ON assignments(project, person)");
    const auto dao = drogon::sync_wait(ConstrainedDao<AssignmentFields>::init(database));
    const auto indexes = drogon::sync_wait(dao.executeSql(
        "SELECT name FROM pragma_index_list('assignments') WHERE [unique] = 1"));
    REQUIRE(indexes.size() == 2);
    CHECK(std::any_of(indexes.begin(), indexes.end(), [](const auto& row) {
        return row[0].template as<std::string>() == "existing_assignment";
    }));
    drogon::sync_wait(ConstrainedDao<AssignmentFields>::init(database));
    CHECK(drogon::sync_wait(dao.findById(1)));
}

class ComposedDao final : public BaseDao<ComposedDao, ComposedRecord> {
public:
    static drogon::Task<ComposedDao> init(DatabaseBackendPtr database) {
        return finishInitialization(ComposedDao(std::move(database)));
    }

private:
    explicit ComposedDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

TEST_CASE("Nested field annotations preserve uniqueness and foreign loading", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    const auto dao = drogon::sync_wait(ComposedDao::init(database));
    const auto owner = drogon::sync_wait(users.create({{"owner", "hash"}}));
    const auto id = drogon::sync_wait(dao.create({{{}, owner}}));
    const auto record = drogon::sync_wait(dao.findById(id, withForeign<&ComposedRecord::owner_id>(users)));
    REQUIRE(record);
    CHECK(record->label.value.value.value == "first");
    REQUIRE(record->owner_id.value.value.record());
    CHECK(record->owner_id.value.value.record()->id == owner);
    CHECK_THROWS(drogon::sync_wait(dao.create({{"first", {}}})));
    CHECK_THROWS(drogon::sync_wait(dao.create({{"other", owner}})));
    CHECK_THROWS(drogon::sync_wait(dao.create({{"other", owner + 1}})));
    CHECK(drogon::sync_wait(dao.create({{"other", {}}})) > id);
    const auto indexes = drogon::sync_wait(dao.executeSql(
        "SELECT 1 FROM pragma_index_list('composed_records') WHERE origin = 'c'"));
    CHECK(indexes.size() == 2); // Only the two timestamp indexes are ordinary indexes.
}

TEST_CASE("DAO transactions commit or roll back typed operations across tables", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    const auto reviewers = drogon::sync_wait(IndexedDao::init(database));
    const auto posts = drogon::sync_wait(PostDao::init(database));
    const auto other = drogon::sync_wait(UserDao::init(openTestDatabase(":memory:")));
    std::int64_t author_id = 0;
    drogon::sync_wait(database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        author_id = co_await users.create(tx, {{"author", "hash"}});
        auto post_id = co_await posts.create(tx, {{"post", author_id, {}}});
        const auto discarded = co_await posts.create(tx, {{"discard", author_id, {}}});
        CHECK(co_await posts.deleteById(tx, discarded));
        auto post = co_await posts.findById(tx, post_id, withForeign<&PostRecord::author_id>(users));
        REQUIRE(post);
        REQUIRE(post->author_id.value.record());
        post->title = "edited";
        CHECK(co_await posts.update(tx, *post));
        const auto rows = co_await posts.find(tx, 1, 0, where<&PostRecord::title>("edited"));
        CHECK(rows.size() == 1);
        CHECK(co_await posts.count(tx, where<&PostRecord::author_id>(author_id)) == 1);
        const auto raw = co_await posts.executeSql(tx, "SELECT title FROM posts WHERE id = ?", post_id);
        CHECK(raw[0][0].as<std::string>() == "edited");
    }));
    CHECK(drogon::sync_wait(users.findById(author_id)));
    CHECK(drogon::sync_wait(posts.count(where<&PostRecord::title>("edited"))) == 1);
    CHECK_THROWS(drogon::sync_wait(database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        const auto rolled_id = co_await users.create(tx, {{"rolled-back", "hash"}});
        co_await posts.create(tx, {{"rolled-back", rolled_id, {}}});
        co_await posts.create(tx, {{"invalid", 999, {}}});
    })));
    CHECK_FALSE(drogon::sync_wait(users.findByUsername("rolled-back")));
    CHECK(drogon::sync_wait(posts.count(where<&PostRecord::title>("rolled-back"))) == 0);
    CHECK_THROWS_WITH(drogon::sync_wait(database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        co_await other.findById(tx, author_id);
    })), "DAO and transaction must use the same database backend");
}

TEST_CASE("A competing DAO write waits for rollback and commits independently", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    std::promise<void> inserted;
    std::promise<void> release;
    auto transaction = std::async(std::launch::async, [&] {
        runOnEventLoop(database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            co_await users.create(tx, {{"rolled-back", "hash"}});
            inserted.set_value();
            release.get_future().wait();
            throw std::runtime_error("abort transaction");
        }));
    });
    const auto ready = inserted.get_future().wait_for(2s);
    if (ready != std::future_status::ready) {
        release.set_value();
        transaction.get();
        FAIL("transaction did not reach its first write");
    }

    std::promise<void> started;
    auto outside = std::async(std::launch::async, [&] {
        const auto write = [&]() -> drogon::Task<std::int64_t> {
            started.set_value();
            co_return co_await users.create({{"outside", "hash"}});
        };
        return runOnEventLoop(write());
    });
    started.get_future().wait();
    const auto blocked = outside.wait_for(50ms);
    release.set_value();
    CHECK(blocked == std::future_status::timeout);
    CHECK_THROWS_WITH(transaction.get(), "abort transaction");
    CHECK(drogon::sync_wait(users.findById(outside.get())));
    CHECK_FALSE(drogon::sync_wait(users.findByUsername("rolled-back")));
}

TEST_CASE("A SQLite backend with a failed rollback is unavailable", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    CHECK_THROWS(drogon::sync_wait(database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        co_await users.executeSql(tx, "ROLLBACK");
        throw std::runtime_error("original failure");
    })));
    CHECK_THROWS_WITH(drogon::sync_wait(users.find(1)), "SQLite backend unavailable after failed rollback");
}

template <auto Member, typename TargetDao>
concept SupportsForeignFind = requires(const PostDao& posts, const TargetDao& target) {
    posts.findById(1, withForeign<Member>(target));
};

static_assert(SupportsForeignFind<&PostRecord::author_id, UserDao>);
static_assert(!SupportsForeignFind<&PostRecord::title, UserDao>);
static_assert(!SupportsForeignFind<&PostRecord::author_id, IndexedDao>);
static_assert(!SupportsForeignFind<&UserRecord::username, UserDao>);

template <auto Member, typename Dao>
concept SupportsForeignPage = requires(const PostDao& posts, const Dao& target) {
    posts.find(withForeign<Member>(target));
};

static_assert(SupportsForeignPage<&PostRecord::author_id, UserDao>);
static_assert(!SupportsForeignPage<&PostRecord::author_id, IndexedDao>);
static_assert(!SupportsForeignPage<&PostRecord::title, UserDao>);
static_assert(!SupportsForeignPage<&UserRecord::username, UserDao>);

TEST_CASE("Foreign selections load one ordered page with shared and nullable references", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    const auto reviewers = drogon::sync_wait(IndexedDao::init(database));
    const auto posts = drogon::sync_wait(PostDao::init(database));
    const auto author = drogon::sync_wait(users.create({{"author", "hash"}}));
    drogon::sync_wait(reviewers.create({{7, "reviewer"}}));
    drogon::sync_wait(posts.create({{"first", author, {}}}));
    const auto second = drogon::sync_wait(posts.create({{"second", author, 7}}));
    const auto third = drogon::sync_wait(posts.create({{"third", author, {}}}));

    const auto page = drogon::sync_wait(posts.find(2, 1,
        orderBy<&PostRecord::id>(),
        withForeign<&PostRecord::author_id>(users),
        withForeign<&PostRecord::reviewer_id>(reviewers)));
    REQUIRE(page.size() == 2);
    CHECK(page[0].id == second);
    CHECK(page[1].id == third);
    for (const auto& post : page) {
        REQUIRE(post.author_id.value.record());
        CHECK(post.author_id.value.record()->username.value == "author");
    }
    REQUIRE(page[0].reviewer_id.record());
    CHECK(page[0].reviewer_id.record()->category.value == "reviewer");
    CHECK(page[1].isLoaded<&PostRecord::reviewer_id>());
    CHECK_FALSE(page[1].reviewer_id.record());

    const auto otherUsers = drogon::sync_wait(UserDao::init(openTestDatabase(":memory:")));
    CHECK_THROWS_WITH(drogon::sync_wait(posts.find(2, 100,
        withForeign<&PostRecord::author_id>(otherUsers))), "foreign DAOs must use the same database backend");
}

TEST_CASE("Foreign page batches resolve uncommitted keys in the caller transaction", "[users]") {
    const auto database = openTestDatabase(":memory:");
    const auto users = drogon::sync_wait(UserDao::init(database));
    const auto reviewers = drogon::sync_wait(IndexedDao::init(database));
    const auto posts = drogon::sync_wait(PostDao::init(database));

    runOnEventLoop(database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        for (int index = 0; index <= 500; ++index) {
            const auto username = "author-" + std::to_string(index);
            const auto author = co_await users.create(tx, {{username, "hash"}});
            co_await posts.create(tx, {{username, author, {}}});
        }
        const auto page = co_await posts.find(tx, 501, 0, orderBy<&PostRecord::id>(),
            withForeign<&PostRecord::author_id>(users));
        REQUIRE(page.size() == 501);
        for (const auto& post : page) {
            REQUIRE(post.author_id.value.record());
            CHECK(post.author_id.value.record()->username.value == post.title);
        }
    }));
}

TEST_CASE("Foreign record fields enforce references and load related records on demand", "[users]") {
    TemporaryDirectory temporary;
    const auto database = openTestDatabase(temporary.path() / "relations.sqlite3");
    const auto users = drogon::sync_wait(UserDao::init(database));
    const auto reviewers = drogon::sync_wait(IndexedDao::init(database));
    const auto author = drogon::sync_wait(users.create({{"author", "hash"}}));
    drogon::sync_wait(reviewers.create({{7, "reviewer"}}));
    const auto posts = drogon::sync_wait(PostDao::init(database));
    CHECK_THROWS(drogon::sync_wait(posts.create({{"invalid", author + 1, {}}})));
    const auto id = drogon::sync_wait(posts.create({{"first", author, {}}}));
    auto post = drogon::sync_wait(posts.findById(id));
    REQUIRE(post);
    CHECK(post->author_id.value.key() == author);
    const auto with_null = drogon::sync_wait(posts.findById(
        id, withForeign<&PostRecord::reviewer_id>(reviewers)));
    REQUIRE(with_null);
    CHECK_FALSE(with_null->reviewer_id.record());
    CHECK_FALSE(drogon::sync_wait(posts.findById(
        id + 1, withForeign<&PostRecord::author_id>(users))));
    CHECK_FALSE(post->author_id.value.record());
    CHECK_FALSE(drogon::sync_wait(reviewers.loadForeign(post->reviewer_id)));
    CHECK(drogon::sync_wait(users.loadForeign(post->author_id.value)));
    REQUIRE(post->author_id.value.record());
    CHECK(post->author_id.value.record()->username.value == "author");
    post->reviewer_id.setKey(7);
    CHECK(drogon::sync_wait(posts.update(*post)));
    const auto selected_author = drogon::sync_wait(posts.findById(
        id, withForeign<&PostRecord::author_id>(users)));
    REQUIRE(selected_author);
    CHECK(selected_author->author_id.value.record());
    CHECK_FALSE(selected_author->reviewer_id.record());
    const auto selected_both = drogon::sync_wait(posts.findById(
        id, withForeign<&PostRecord::author_id>(users), withForeign<&PostRecord::reviewer_id>(reviewers)));
    REQUIRE(selected_both);
    REQUIRE(selected_both->author_id.value.record());
    REQUIRE(selected_both->reviewer_id.record());
    CHECK(selected_both->author_id.value.record()->username.value == "author");
    CHECK(selected_both->reviewer_id.record()->category.value == "reviewer");
    const auto other_users = drogon::sync_wait(UserDao::init(openTestDatabase(temporary.path() / "other.sqlite3")));
    CHECK_THROWS_WITH(drogon::sync_wait(posts.findById(
        id, withForeign<&PostRecord::author_id>(other_users))), "foreign DAOs must use the same database backend");
    const auto matches = drogon::sync_wait(posts.find(where<&PostRecord::reviewer_id>(7)));
    REQUIRE(matches.size() == 1);
    CHECK_FALSE(matches[0].reviewer_id.record());
    CHECK_FALSE(matches[0].reviewer_id.key());
    CHECK_FALSE(matches[0].isLoaded<&PostRecord::reviewer_id>());
    CHECK(matches[0].author_id.value.key() == author);
    CHECK(drogon::sync_wait(posts.update(matches[0])));
    const auto projected = drogon::sync_wait(posts.find(
        select<&PostRecord::reviewer_id>(), where<&PostRecord::id>(id)));
    REQUIRE(projected.size() == 1);
    CHECK(projected[0].isLoaded<&PostRecord::reviewer_id>());
    CHECK(projected[0].reviewer_id.key() == 7);
    CHECK(drogon::sync_wait(reviewers.loadForeign(post->reviewer_id)));
    REQUIRE(post->reviewer_id.record());
    CHECK(post->reviewer_id.record()->category.value == "reviewer");
    post->reviewer_id.setKey(8);
    CHECK_FALSE(post->reviewer_id.record());
    CHECK_FALSE(drogon::sync_wait(reviewers.loadForeign(post->reviewer_id)));
    CHECK_THROWS(drogon::sync_wait(posts.update(*post)));
    CHECK_THROWS(drogon::sync_wait(users.deleteById(author)));
    CHECK(drogon::sync_wait(posts.deleteById(id)));
    CHECK(drogon::sync_wait(users.deleteById(author)));
}


TEST_CASE("SQLite startup adds nullable foreign columns and rejects inconsistent references", "[users]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "relations.sqlite3";
    const auto database = openTestDatabase(path);
    const auto users = drogon::sync_wait(UserDao::init(database));
    CHECK_THROWS_WITH(drogon::sync_wait(PostDao::init(database)),
                      "Schema mismatch for posts: invalid foreign key target for reviewer_id; explicit migration required");
    const auto reviewers = drogon::sync_wait(IndexedDao::init(database));
    const auto author = drogon::sync_wait(users.create({{"author", "hash"}}));
    const auto legacy = drogon::orm::DbClient::newSqlite3Client("filename='" + path.string() + "'", 1);
    legacy->execSqlSync("CREATE TABLE posts (title TEXT NOT NULL, author_id INTEGER NOT NULL "
                        "REFERENCES users(id) ON UPDATE RESTRICT ON DELETE RESTRICT, "
                        "id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL)");
    legacy->execSqlSync("INSERT INTO posts (title, author_id) VALUES ('existing', ?)", author);
    const auto posts = drogon::sync_wait(PostDao::init(database));
    const auto rows = drogon::sync_wait(posts.find(where<&PostRecord::reviewer_id>(std::nullopt)));
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows[0].reviewer_id.key());
    legacy->execSqlSync("PRAGMA foreign_keys = OFF");
    legacy->execSqlSync("UPDATE posts SET reviewer_id = 99");
    const auto orphan = drogon::sync_wait(posts.findById(
        *rows[0].id, withForeign<&PostRecord::reviewer_id>(reviewers)));
    REQUIRE(orphan);
    CHECK(orphan->reviewer_id.key() == 99);
    CHECK_FALSE(orphan->reviewer_id.record());
    const auto orphans = drogon::sync_wait(posts.find(withForeign<&PostRecord::reviewer_id>(reviewers)));
    REQUIRE(orphans.size() == 1);
    CHECK(orphans.front().reviewer_id.key() == 99);
    CHECK_FALSE(orphans.front().reviewer_id.record());

    CHECK_THROWS_WITH(drogon::sync_wait(PostDao::init(database)),
                      "Schema mismatch for posts: orphaned foreign key rows; explicit migration required");
    legacy->execSqlSync("DROP TABLE posts");
    legacy->execSqlSync("CREATE TABLE posts (title TEXT NOT NULL, author_id INTEGER NOT NULL, "
                        "reviewer_id INTEGER, id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL)");
    CHECK_THROWS_WITH(drogon::sync_wait(PostDao::init(database)),
                      "Schema mismatch for posts: foreign key for author_id; explicit migration required");
}

TEST_CASE("Base DAO generates and validates annotated SQLite schemas", "[users]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "schema.sqlite3";
    const auto database = openTestDatabase(path);
    std::latch start(2);
    auto userInit = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return drogon::sync_wait(UserDao::init(database));
    });
    start.arrive_and_wait();
    const auto firstIndex = drogon::sync_wait(IndexedDao::init(database));
    const auto users = userInit.get();
    CHECK(drogon::sync_wait(users.find(1)).empty());
    const auto id = drogon::sync_wait(users.create({{"current", "hash", "demo"}}));
    const auto reopenedUsers = drogon::sync_wait(UserDao::init(database));
    CHECK(drogon::sync_wait(reopenedUsers.findById(id))->seed_key.value == "demo");

    CHECK(drogon::sync_wait(firstIndex.find(1)).empty());
    const auto indexed = drogon::sync_wait(IndexedDao::init(database));
    const auto indexes = drogon::sync_wait(indexed.executeSql(
        "SELECT name FROM pragma_index_list('indexed_records') WHERE origin = 'c'"));
    REQUIRE(indexes.size() == 4);
    drogon::sync_wait(indexed.create({{7, "sample"}}));
    CHECK(drogon::sync_wait(indexed.find(where<&IndexedRecord::category>("sample"))).size() == 1);


}

TEST_CASE("Base DAO adds eligible columns transactionally and preserves removed columns", "[users]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "upgrade.sqlite3";
    const auto legacy = drogon::orm::DbClient::newSqlite3Client("filename='" + path.string() + "'", 1);
    legacy->execSqlSync("CREATE TABLE indexed_records (id INTEGER PRIMARY KEY NOT NULL, category TEXT NOT NULL)");
    legacy->execSqlSync("INSERT INTO indexed_records VALUES (7, 'original')");
    legacy->execSqlSync("CREATE TABLE unrelated (category TEXT)");
    const auto database = openTestDatabase(path);
    const auto users = drogon::sync_wait(UserDao::init(database));
    drogon::sync_wait(users.create({{"unaffected", "hash", std::nullopt}}));
    legacy->execSqlSync("CREATE INDEX idx_15_indexed_records_category ON unrelated (category)");
    CHECK_THROWS_WITH(drogon::sync_wait(IndexedDao::init(database)),
                      "Schema mismatch for indexed_records: invalid index idx_15_indexed_records_category; explicit migration required");
    CHECK(legacy->execSqlSync("SELECT * FROM pragma_table_info('indexed_records')").size() == 2);
    CHECK(drogon::sync_wait(users.count(where<&UserRecord::password_hash>("hash"))) == 1);
    legacy->execSqlSync("DROP INDEX idx_15_indexed_records_category");

    const auto dao = drogon::sync_wait(IndexedDao::init(database));
    const auto original = drogon::sync_wait(dao.findById(7));
    REQUIRE(original);
    CHECK(original->category.value == "original");
    CHECK_FALSE(original->note);
    CHECK_FALSE(original->active.value);
    CHECK(original->priority.value == 5);
    CHECK(original->ratio.value == 1.25);
    constexpr auto initial = detail::parseDaoDateTime("2027-04-01T12:34:56.789Z");
    CHECK(original->starts_at.value == initial);
    CHECK_FALSE(original->ends_at);
    CHECK(drogon::sync_wait(dao.find(where<&IndexedRecord::starts_at>(initial))).size() == 1);
    auto edited = *original;
    edited.starts_at.value = initial + 1h;
    edited.ends_at = initial + 2h;
    CHECK(drogon::sync_wait(dao.update(edited)));
    const auto updated = drogon::sync_wait(dao.find(where<&IndexedRecord::ends_at>(initial + 2h)));
    REQUIRE(updated.size() == 1);
    CHECK(updated.front().starts_at.value == initial + 1h);

    const auto reopened = drogon::sync_wait(IndexedDao::init(database));
    CHECK(drogon::sync_wait(reopened.find(where<&IndexedRecord::label>("O'Brien"))).size() == 1);
    drogon::sync_wait(reopened.create({{8, "new"}}));
    const auto created = drogon::sync_wait(reopened.findById(8));
    REQUIRE(created);
    CHECK(created->label.value.value == "O'Brien");
    CHECK(created->starts_at.value == initial);
    CHECK_FALSE(created->ends_at);

    legacy->execSqlSync("ALTER TABLE indexed_records ADD COLUMN removed_field TEXT");
    legacy->execSqlSync("UPDATE indexed_records SET removed_field = 'preserved' WHERE id = 7");
    CHECK_THROWS_WITH(drogon::sync_wait(IndexedDao::init(database)),
                      "Schema mismatch for indexed_records: extra column removed_field; explicit migration required");
    CHECK(legacy->execSqlSync("SELECT removed_field FROM indexed_records WHERE id = 7")[0][0].as<std::string>() ==
          "preserved");
}

TEST_CASE("Base DAO filters by record members before paginating", "[users]") {
    TemporaryDirectory temporary;
    const auto users = drogon::sync_wait(UserDao::init(openTestDatabase(temporary.path() / "filters.sqlite3")));
    const std::string punctuation = "charlie' --";
    drogon::sync_wait(users.create({{"alice", "shared", "demo"}}));
    drogon::sync_wait(users.create({{"bob", "shared", std::nullopt}}));
    drogon::sync_wait(users.create({{punctuation, "other", std::nullopt}}));

    const auto demo = drogon::sync_wait(users.find(where<&UserRecord::seed_key>("demo")));
    REQUIRE(demo.size() == 1);
    CHECK(demo[0].username.value == "alice");

    const auto match = drogon::sync_wait(users.find(
        where<&UserRecord::username>(punctuation), where<&UserRecord::seed_key>(std::nullopt)));
    REQUIRE(match.size() == 1);
    CHECK(match[0].username.value == punctuation);

    CHECK(drogon::sync_wait(users.find(
        where<&UserRecord::username>("alice"), where<&UserRecord::seed_key>(std::nullopt))).empty());

    const auto page = drogon::sync_wait(users.find(1, 1, orderBy<&UserRecord::id>(), where<&UserRecord::password_hash>("shared")));
    REQUIRE(page.size() == 1);
    CHECK(page[0].username.value == "bob");
}

static_assert(!SupportsFind<UserDao, decltype(select<&PostRecord::reviewer_id>())>);
static_assert(!SupportsFind<UserDao, decltype(orderBy<&PostRecord::title>())>);

template <auto Member, typename Operand>
concept SupportsOperator = requires(Operand operand) { where<Member>(operand); };

template <typename... Predicates>
concept SupportsUserCount = requires(const UserDao& users, Predicates... predicates) { users.count(predicates...); };

static_assert(SupportsOperator<&UserRecord::id, decltype(gte(1))>);
static_assert(!SupportsOperator<&UserRecord::username, decltype(gte(1))>);
static_assert(!SupportsOperator<&UserRecord::id, decltype(contains("1"))>);
static_assert(!SupportsUserCount<>);
static_assert(!SupportsUserCount<decltype(orderBy<&UserRecord::id>())>);
static_assert(!SupportsUserCount<decltype(where<&PostRecord::id>(1))>);

TEST_CASE("Find and count share typed comparison and membership predicates", "[users]") {
    TemporaryDirectory temporary;
    const auto users = drogon::sync_wait(UserDao::init(openTestDatabase(temporary.path() / "predicates.sqlite3")));
    const auto first = drogon::sync_wait(users.create({{"Release %_notes", "hash", "dev"}}));
    const auto second = drogon::sync_wait(users.create({{"release %_notes", "hash"}}));
    const auto third = drogon::sync_wait(users.create({{"release XXnotes", "hash"}}));

    const auto lower = where<&UserRecord::id>(gte(first));
    const auto upper = where<&UserRecord::id>(lte(second));
    CHECK(drogon::sync_wait(users.find(lower, upper)).size() == 2);
    CHECK(drogon::sync_wait(users.find(1, 0, lower, upper)).size() == 1);
    CHECK(drogon::sync_wait(users.count(lower, upper)) == 2);
    CHECK(drogon::sync_wait(users.count(where<&UserRecord::id>(gt(first)), where<&UserRecord::id>(lt(third)))) == 1);

    const auto literal = where<&UserRecord::username>(contains("%_notes"));
    const auto capital = where<&UserRecord::username>(contains("Release"));
    CHECK(drogon::sync_wait(users.count(literal)) == 2);
    CHECK(drogon::sync_wait(users.count(literal, capital)) == 1);
    CHECK(drogon::sync_wait(users.count(negate(literal))) == 1);
    CHECK(drogon::sync_wait(users.count(negate(where<&UserRecord::seed_key>(contains("dev"))))) == 2);
    CHECK(drogon::sync_wait(users.count(where<&UserRecord::seed_key>(eq(std::nullopt)))) == 2);

    const auto ids = where<&UserRecord::id>(in({first, third}));
    CHECK(drogon::sync_wait(users.find(1, 0, ids)).size() == 1);
    CHECK(drogon::sync_wait(users.count(ids, lower)) == 2);
    const auto empty = where<&UserRecord::id>(in(std::vector<std::int64_t>{}));
    CHECK(drogon::sync_wait(users.count(empty)) == 0);
    CHECK(drogon::sync_wait(users.count(negate(empty))) == 3);
    const auto nullable = where<&UserRecord::seed_key>(in(std::array{
        std::optional<std::string>{"dev"}, std::optional<std::string>{}}));
    CHECK(drogon::sync_wait(users.count(nullable)) == 3);
    CHECK(drogon::sync_wait(users.count(negate(nullable))) == 0);

    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = NULL WHERE id = ?", first));
    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = ? WHERE id IN (?, ?)",
                                      "2026-11-01T01:00:00.000Z", second, third));
    constexpr auto timestamp = detail::parseDaoDateTime("2026-11-01T01:00:00.000Z");
    const auto timed = where<&UserRecord::updated_at>(gte(timestamp));
    CHECK(drogon::sync_wait(users.count(timed)) == 2);
    CHECK(drogon::sync_wait(users.count(negate(timed))) == 1);
}

TEST_CASE("Find projects nullable fields and sorts by recent updates", "[users]") {
    TemporaryDirectory temporary;
    const auto backend = openTestDatabase(temporary.path() / "projection.sqlite3");
    const auto users = drogon::sync_wait(UserDao::init(backend));
    const auto id = drogon::sync_wait(users.create({{"alpha", "hash", "seed"}}));
    const auto next = drogon::sync_wait(users.create({{"beta", "hash"}}));
    const auto third = drogon::sync_wait(users.create({{"gamma", "hash"}}));
    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = ? WHERE id IN (?, ?)",
                                      "2026-11-01T01:00:00.000Z", next, third));
    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = NULL WHERE id = ?", id));

    auto newest = drogon::sync_wait(users.find());
    REQUIRE(newest.size() == 3);
    CHECK(newest[0].id == next);
    CHECK(newest[1].id == third);
    CHECK(newest[2].id == id);
    CHECK_FALSE(newest[0].updated_at);
    CHECK_FALSE(newest[2].isLoaded<&UserRecord::seed_key>());
    CHECK(newest[2].isLoaded<&UserRecord::id>());
    newest[2].password_hash = "changed";
    CHECK(drogon::sync_wait(users.update(newest[2])));
    CHECK(drogon::sync_wait(users.findById(id))->seed_key.value == "seed");

    auto selected = drogon::sync_wait(users.find(
        select<&UserRecord::seed_key>(), orderBy<&UserRecord::username>(SortDirection::desc)));
    REQUIRE(selected.size() == 3);
    CHECK(selected[0].id == third);
    CHECK(selected[0].isLoaded<&UserRecord::seed_key>());
    CHECK_FALSE(selected[0].seed_key.value);
    CHECK(selected[2].id == id);
    selected[2].seed_key.value.reset();
    CHECK(drogon::sync_wait(users.update(selected[2])));
    CHECK_FALSE(drogon::sync_wait(users.findById(id))->seed_key.value);
    CHECK(drogon::sync_wait(users.find(selectAll())).front().isLoaded<&UserRecord::updated_at>());

    const auto indexes = drogon::sync_wait(users.executeSql(
        "SELECT info.name, info.[desc] FROM pragma_index_list('users') AS idx "
        "JOIN pragma_index_xinfo(idx.name) AS info WHERE info.[key] = 1 "
        "AND info.name IN ('created_at', 'updated_at')"));
    REQUIRE(indexes.size() == 2);
    for (const auto& index : indexes) CHECK(index[1].as<int>() == 1);

    const auto disabled = drogon::sync_wait(UntimedUserDao::init(
        openTestDatabase(temporary.path() / "untimed.sqlite3")));
    const auto first = drogon::sync_wait(disabled.create({{"zeta", "hash"}}));
    drogon::sync_wait(disabled.create({{"alpha", "hash"}}));
    CHECK(drogon::sync_wait(disabled.find(1)).front().id == first);
    CHECK_THROWS_AS(drogon::sync_wait(disabled.find(orderBy<&UserRecord::updated_at>())), std::invalid_argument);
    CHECK(drogon::sync_wait(disabled.executeSql(
        "SELECT name FROM pragma_index_list('users') WHERE origin = 'c'")).empty());
}

TEST_CASE("Base DAO provides asynchronous CRUD and bound SQL", "[users]") {
    TemporaryDirectory temporary;
    const auto users = drogon::sync_wait(UserDao::init(openTestDatabase(temporary.path() / "crud.sqlite3")));
    CHECK(drogon::sync_wait(users.find(1)).empty());
    CHECK_FALSE(drogon::sync_wait(users.findById(99)));
    const auto bobId = drogon::sync_wait(users.create({{"bob", "hash", std::nullopt}}));
    const auto aliceId = drogon::sync_wait(users.create({{"alice", "hash", "seed"}}));
    CHECK(aliceId > bobId);
    CHECK_THROWS(drogon::sync_wait(users.create({{"bob", "other", std::nullopt}})));
    CHECK_THROWS_AS(drogon::sync_wait(users.find(0)), std::invalid_argument);
    const auto page = drogon::sync_wait(users.find(1, 1, select<&UserRecord::seed_key>(), orderBy<&UserRecord::id>()));
    REQUIRE(page.size() == 1);
    CHECK(page[0].username.value == "alice");
    CHECK(page[0].seed_key.value == "seed");
    const auto alice = drogon::sync_wait(users.findById(aliceId));
    REQUIRE(alice);
    CHECK(alice->seed_key.value == "seed");

    UserRecord missing{{"missing", "new hash", std::nullopt}};
    missing.id = 99;
    CHECK_FALSE(drogon::sync_wait(users.update(missing)));
    UserRecord conflict{{"bob", "conflict", "seed"}};
    conflict.id = bobId;
    CHECK_THROWS(drogon::sync_wait(users.update(conflict)));
    auto changed = *alice;
    changed.password_hash = "new hash";
    changed.seed_key.value.reset();
    CHECK(drogon::sync_wait(users.update(changed)));
    const auto updated = drogon::sync_wait(users.findById(aliceId));
    REQUIRE(updated);
    CHECK(updated->password_hash == "new hash");
    CHECK_FALSE(updated->seed_key.value);
    const auto bob = drogon::sync_wait(users.findById(bobId));
    REQUIRE(bob);
    CHECK(bob->password_hash == "hash");
    CHECK(drogon::sync_wait(users.count(where<&UserRecord::id>(in({aliceId, bobId})))) == 2);

    trantor::EventLoopThread thread;
    thread.run();
    const auto checkLoop = [&]() -> drogon::Task<std::pair<bool, bool>> {
        auto* loop = thread.getLoop();
        co_await drogon::switchThreadCoro(loop);
        const auto result = co_await users.executeSql("SELECT username FROM users WHERE username = ?", "alice");
        const bool success = result.size() == 1 && loop->isInLoopThread();
        try {
            co_await users.executeSql("SELECT missing_column FROM users");
        } catch (const std::exception&) {
            co_return std::pair{success, loop->isInLoopThread()};
        }
        co_return std::pair{success, false};
    };
    const auto [success, error] = drogon::sync_wait(checkLoop());
    CHECK(success);
    CHECK(error);
    CHECK(drogon::sync_wait(users.deleteById(aliceId)));
    CHECK_FALSE(drogon::sync_wait(users.deleteById(aliceId)));
    CHECK(drogon::sync_wait(users.count(where<&UserRecord::password_hash>("hash"))) == 1);
}

TEST_CASE("Generated IDs and managed timestamps cover custom demo writes and opt-out", "[users]") {
    TemporaryDirectory temporary;
    const auto users = drogon::sync_wait(UserDao::init(openTestDatabase(temporary.path() / "managed.sqlite3")));
    CHECK(drogon::sync_wait(users.insertDemo("demo", "hash")));
    auto first = drogon::sync_wait(users.findByUsername("demo"));
    REQUIRE(first);
    REQUIRE(first->id);
    REQUIRE(first->created_at);
    const auto columns = drogon::sync_wait(users.executeSql(
        "SELECT name FROM pragma_table_info('users')"));
    std::vector<std::string> names;
    for (const auto& column : columns) names.push_back(column[0].as<std::string>());
    CHECK(names == std::vector<std::string>{"username", "password_hash", "seed_key", "id", "deleted_at", "created_at", "updated_at"});
    CHECK(first->updated_at == first->created_at);
    CHECK(drogon::sync_wait(users.find(where<&UserRecord::id>(*first->id))).size() == 1);
    CHECK(drogon::sync_wait(users.find(where<&UserRecord::created_at>(*first->created_at))).size() == 1);
    const auto json = glz::write_json(*first->created_at);
    REQUIRE(json);
    CHECK(json->ends_with("Z\""));
    const auto decoded = glz::read_json<DaoDateTime>(*json);
    REQUIRE(decoded);
    CHECK(*decoded == *first->created_at);
    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = 'invalid' WHERE id = ?", *first->id));
    CHECK_THROWS_WITH(drogon::sync_wait(users.findById(*first->id)), "invalid DAO UTC datetime");

    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = NULL WHERE id = ?", *first->id));
    CHECK_FALSE(drogon::sync_wait(users.insertDemo("ignored", "hash")));
    CHECK_FALSE(drogon::sync_wait(users.findById(*first->id))->updated_at);
    drogon::sync_wait(users.replaceDemo("renamed", "new hash"));
    const auto renamed = drogon::sync_wait(users.findByUsername("renamed"));
    REQUIRE(renamed);
    CHECK(renamed->id == first->id);
    CHECK(renamed->created_at == first->created_at);
    CHECK(renamed->updated_at);
    CHECK(drogon::sync_wait(users.deleteById(*first->id)));
    CHECK(drogon::sync_wait(users.create({{"next", "hash"}})) > *first->id);

    const auto disabled = drogon::sync_wait(UntimedUserDao::init(
        openTestDatabase(temporary.path() / "disabled.sqlite3")));
    const auto id = drogon::sync_wait(disabled.create({{"plain", "hash"}}));
    const auto plain = drogon::sync_wait(disabled.findById(id));
    REQUIRE(plain);
    CHECK_FALSE(plain->created_at);
    CHECK_FALSE(plain->updated_at);
    CHECK(drogon::sync_wait(disabled.executeSql(
        "SELECT name FROM pragma_table_info('users') WHERE name IN ('created_at', 'updated_at')")).empty());
}

TEST_CASE("Soft delete hides a row and restore preserves its original creation time", "[users]") {
    const auto users = drogon::sync_wait(UserDao::init(openTestDatabase(":memory:")));
    const auto id = drogon::sync_wait(users.create({{"alice", "hash"}}));
    const auto active = drogon::sync_wait(users.findById(id));
    REQUIRE(active);
    REQUIRE(active->created_at);
    CHECK(active->updated_at == active->created_at);
    CHECK_FALSE(active->deleted_at);
    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = NULL WHERE id = ?", id));
    REQUIRE(drogon::sync_wait(users.update(*active)));
    REQUIRE(drogon::sync_wait(users.findById(id))->updated_at);
    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = NULL WHERE id = ?", id));

    CHECK(drogon::sync_wait(users.softDeleteById(id)));
    CHECK_FALSE(drogon::sync_wait(users.softDeleteById(id)));
    CHECK_FALSE(drogon::sync_wait(users.softDeleteById(id + 100)));
    CHECK_FALSE(drogon::sync_wait(users.update(*active)));
    CHECK_FALSE(drogon::sync_wait(users.findById(id)));
    CHECK_FALSE(drogon::sync_wait(users.findByUsername("alice")));
    CHECK(drogon::sync_wait(users.count(where<&UserRecord::id>(id))) == 0);
    CHECK(drogon::sync_wait(users.count(includeDeleted(), where<&UserRecord::id>(id))) == 1);
    CHECK_THROWS(drogon::sync_wait(users.create({{"alice", "other"}})));

    const auto deleted = drogon::sync_wait(users.findById(id, includeDeleted()));
    REQUIRE(deleted);
    REQUIRE(deleted->deleted_at);
    CHECK(deleted->updated_at == deleted->deleted_at);
    CHECK(deleted->created_at == active->created_at);
    const auto bob = drogon::sync_wait(users.create({{"bob", "hash"}}));
    const auto page = drogon::sync_wait(users.find(1, 0, orderBy<&UserRecord::id>()));
    REQUIRE(page.size() == 1);
    CHECK(page.front().id == bob);
    const auto tombstones = drogon::sync_wait(users.find(includeDeleted(),
        select<&UserRecord::deleted_at>(), where<&UserRecord::deleted_at>(*deleted->deleted_at)));
    REQUIRE(tombstones.size() == 1);
    CHECK(tombstones.front().deleted_at == deleted->deleted_at);

    drogon::sync_wait(users.executeSql("UPDATE users SET updated_at = NULL WHERE id = ?", id));
    CHECK(drogon::sync_wait(users.restoreById(id)));
    CHECK_FALSE(drogon::sync_wait(users.restoreById(id)));
    CHECK_FALSE(drogon::sync_wait(users.restoreById(id + 100)));
    const auto restored = drogon::sync_wait(users.findById(id));
    REQUIRE(restored);
    CHECK(restored->updated_at);
    CHECK(restored->created_at == active->created_at);
    CHECK_FALSE(restored->deleted_at);
    CHECK(drogon::sync_wait(users.softDeleteById(id)));
    CHECK(drogon::sync_wait(users.deleteById(id)));
    CHECK_FALSE(drogon::sync_wait(users.findById(id, includeDeleted())));
}

TEST_CASE("Soft deletion on an existing untimed table migrates and rolls back atomically", "[users]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "legacy.sqlite3";
    const auto legacy = drogon::orm::DbClient::newSqlite3Client("filename='" + path.string() + "'", 1);
    legacy->execSqlSync("CREATE TABLE users (username TEXT NOT NULL UNIQUE, password_hash TEXT NOT NULL, "
                        "seed_key TEXT UNIQUE, id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL)");
    legacy->execSqlSync("INSERT INTO users (username, password_hash) VALUES ('legacy', 'hash')");
    const auto database = openTestDatabase(path);
    const auto users = drogon::sync_wait(UntimedUserDao::init(database));
    const auto migrated = drogon::sync_wait(users.findById(1));
    REQUIRE(migrated);
    CHECK_FALSE(migrated->deleted_at);
    CHECK_THROWS_WITH(runOnEventLoop(users.transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        CHECK(co_await users.softDeleteById(tx, 1));
        CHECK_FALSE(co_await users.findById(tx, 1));
        const auto deleted = co_await users.findById(tx, 1, includeDeleted());
        REQUIRE(deleted);
        CHECK(deleted->deleted_at);
        CHECK_FALSE(deleted->updated_at);
        CHECK((co_await users.count(tx, includeDeleted())) == 1);
        CHECK(co_await users.restoreById(tx, 1));
        CHECK(co_await users.softDeleteById(tx, 1));
        throw std::runtime_error("rollback soft delete");
    })), "rollback soft delete");
    CHECK(drogon::sync_wait(users.findById(1)));
}

template <typename Status>
class StatusDao final : public BaseDao<StatusDao<Status>, DaoRecord<StatusFields<Status>>> {
    using Base = BaseDao<StatusDao<Status>, DaoRecord<StatusFields<Status>>>;
public:
    static drogon::Task<StatusDao> init(DatabaseBackendPtr database) {
        return Base::finishInitialization(StatusDao(std::move(database)));
    }

private:
    explicit StatusDao(DatabaseBackendPtr database) : Base(std::move(database)) {}
};

TEST_CASE("DAO reflects plain enums and enforces their stored labels", "[users]") {
    TemporaryDirectory temporary;
    const auto database = openTestDatabase(temporary.path() / "status.sqlite3");
    database->client()->execSqlSync("CREATE TABLE status_records "
                                    "(title TEXT NOT NULL, id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL)");
    database->client()->execSqlSync("INSERT INTO status_records (title) VALUES ('existing')");
    const auto dao = drogon::sync_wait(StatusDao<PostStatus>::init(database));
    auto existing = drogon::sync_wait(dao.findById(1));
    REQUIRE(existing);
    CHECK(existing->status.value == PostStatus::draft);
    CHECK_FALSE(existing->previous_status);
    existing->status.value = PostStatus::published;
    existing->previous_status = PostStatus::draft;
    CHECK(drogon::sync_wait(dao.update(*existing)));
    using Record = DaoRecord<StatusFields<PostStatus>>;
    CHECK(drogon::sync_wait(dao.count(where<&Record::status>(in({PostStatus::published, PostStatus::draft})))) == 1);
    CHECK(drogon::sync_wait(dao.find(where<&Record::status>(PostStatus::published))).size() == 1);
    CHECK(drogon::sync_wait(dao.find(where<&Record::previous_status>(PostStatus::draft))).size() == 1);
    const auto id = drogon::sync_wait(dao.create({{"new"}}));
    CHECK(drogon::sync_wait(dao.findById(id))->status.value == PostStatus::draft);
    CHECK_THROWS_AS(drogon::sync_wait(dao.create({{"bad", static_cast<PostStatus>(99)}})), std::invalid_argument);
    CHECK_THROWS(drogon::sync_wait(dao.executeSql("UPDATE status_records SET status = 'bogus' WHERE id = 1")));
    CHECK_THROWS_WITH(drogon::sync_wait(StatusDao<ChangedStatus>::init(database)),
                      "Schema mismatch for status_records: enum constraint for status; explicit migration required");
    drogon::sync_wait(dao.executeSql("PRAGMA ignore_check_constraints = ON"));
    drogon::sync_wait(dao.executeSql("UPDATE status_records SET status = 'bogus' WHERE id = 1"));
    CHECK_THROWS_WITH(drogon::sync_wait(dao.findById(1)), "unknown DAO enum label: bogus");
}

TEST_CASE("Drogon rows map named columns and respect nullable members", "[users]") {
    const auto db = drogon::orm::DbClient::newSqlite3Client("filename=:memory:", 1);
    const auto result = db->execSqlSync("SELECT NULL AS label, 42 AS count, 'extra' AS ignored");
    const auto row = rowToStruct<SampleRow>(result[0]);
    CHECK(row.count == 42);
    CHECK_FALSE(row.label);

    const auto missing = db->execSqlSync("SELECT 42 AS count");
    CHECK_THROWS(rowToStruct<SampleRow>(missing[0]));
    const auto requiredNull = db->execSqlSync("SELECT NULL AS count, 'label' AS label");
    CHECK_THROWS_AS(rowToStruct<SampleRow>(requiredNull[0]), std::runtime_error);
}

TEST_CASE("Listener settings resolve defaults, JSON, dotenv, and process overrides", "[config]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "config.json";
    const auto env = temporary.path() / ".env";
    SavedEnvironment address("APP_BIND_ADDRESS");
    SavedEnvironment port("APP_PORT");
    SavedEnvironment connections("APP_MAX_CONNECTIONS");
    REQUIRE(unsetenv("APP_BIND_ADDRESS") == 0);
    REQUIRE(unsetenv("APP_PORT") == 0);
    REQUIRE(unsetenv("APP_MAX_CONNECTIONS") == 0);

    std::ofstream(path) << R"({"port":8080,"jwt_lifetime_seconds":1800,"log_level":"info"})";
    CHECK(readConfig(path).bindAddress == "127.0.0.1");
    CHECK(readConfig(path).maxConnections == 64);

    std::ofstream(path) << R"({"bind_address":"127.0.0.2","port":8081,"max_connections":32,"jwt_lifetime_seconds":1800,"log_level":"info"})";
    CHECK(readConfig(path).bindAddress == "127.0.0.2");
    CHECK(readConfig(path).port == 8081);
    CHECK(readConfig(path).maxConnections == 32);

    std::ofstream(env) << "APP_BIND_ADDRESS=127.0.0.3\nAPP_PORT=8082\nAPP_MAX_CONNECTIONS=16\n";
    REQUIRE(setenv("APP_PORT", "8083", 1) == 0);
    loadEnvironment(env);
    CHECK(readConfig(path).bindAddress == "127.0.0.3");
    CHECK(readConfig(path).port == 8083);
    CHECK(readConfig(path).maxConnections == 16);

    REQUIRE(setenv("APP_BIND_ADDRESS", "localhost", 1) == 0);
    CHECK_THROWS(readConfig(path));
    REQUIRE(unsetenv("APP_BIND_ADDRESS") == 0);
    REQUIRE(setenv("APP_PORT", "8080x", 1) == 0);
    CHECK_THROWS(readConfig(path));
    REQUIRE(unsetenv("APP_PORT") == 0);
    REQUIRE(setenv("APP_MAX_CONNECTIONS", "0", 1) == 0);
    CHECK_THROWS(readConfig(path));
}

TEST_CASE("Glaze reads and validates config.json", "[config]") {
    TemporaryDirectory temporary;
    SavedEnvironment port("APP_PORT");
    REQUIRE(unsetenv("APP_PORT") == 0);
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
    SavedEnvironment saved("APP_JWT_SECRET");
    const auto path = temporary.path() / ".env";
    REQUIRE(unsetenv("APP_JWT_SECRET") == 0);

    CHECK(loadSigningKey().size() == 32);
    std::ofstream(path) << "APP_JWT_SECRET=" << std::string(64, 'a') << '\n';
    loadEnvironment(path);
    CHECK(loadSigningKey() == std::string(32, static_cast<char>(0xaa)));

    REQUIRE(setenv("APP_JWT_SECRET", std::string(64, '1').c_str(), 1) == 0);
    loadEnvironment(path);
    CHECK(loadSigningKey() == std::string(32, '\x11'));
    REQUIRE(setenv("APP_JWT_SECRET", "invalid", 1) == 0);
    CHECK_THROWS(loadSigningKey());
    REQUIRE(setenv("APP_JWT_SECRET", std::string(64, 'z').c_str(), 1) == 0);
    CHECK_THROWS(loadSigningKey());
}

TEST_CASE("SQLite persists only the hashed demo credential", "[users]") {
    REQUIRE(sodium_init() >= 0);
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "users with spaces.sqlite3";
    const UserService firstRun(drogon::sync_wait(UserDao::init(openTestDatabase(path))));
    const auto credentials = runOnEventLoop(firstRun.seedDemo());
    REQUIRE(credentials.has_value());
    CHECK(credentials->username.starts_with("demo-"));
    CHECK(credentials->password.size() == 32);

    const UserService secondRun(drogon::sync_wait(UserDao::init(openTestDatabase(path))));
    CHECK_FALSE(runOnEventLoop(secondRun.seedDemo()));
    CHECK(authenticate(secondRun, credentials->username, credentials->password));
    CHECK_FALSE(authenticate(secondRun, credentials->username, "wrong"));
    CHECK_FALSE(authenticate(secondRun, "missing", credentials->password));

    std::ifstream database(path, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(database), {}};
    CHECK(raw.find("$argon2id$") != std::string::npos);
    CHECK(raw.find(credentials->password) == std::string::npos);
}

TEST_CASE("Configured demo credentials replace a generated account and survive restart", "[users]") {
    REQUIRE(sodium_init() >= 0);
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "users.sqlite3";
    const UserService users(drogon::sync_wait(UserDao::init(openTestDatabase(path))));
    const auto previous = runOnEventLoop(users.seedDemo());
    REQUIRE(previous);

    runOnEventLoop(users.configureDemo("test_user", "test_password"));
    CHECK_FALSE(authenticate(users, previous->username, previous->password));
    CHECK(authenticate(users, "test_user", "test_password"));

    const UserService reopened(drogon::sync_wait(UserDao::init(openTestDatabase(path))));
    runOnEventLoop(reopened.configureDemo("test_user", "new_password"));
    CHECK_FALSE(authenticate(reopened, "test_user", "test_password"));
    CHECK(authenticate(reopened, "test_user", "new_password"));
    CHECK_FALSE(runOnEventLoop(reopened.seedDemo()));
}

TEST_CASE("Concurrent demo seeding returns credentials to exactly one caller", "[users]") {
    REQUIRE(sodium_init() >= 0);
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "users.sqlite3";
    std::latch start(2);
    const auto seed = [&] {
        start.arrive_and_wait();
        const UserService users(drogon::sync_wait(UserDao::init(openTestDatabase(path))));
        return runOnEventLoop(users.seedDemo());
    };
    auto first = std::async(std::launch::async, seed);
    const auto secondResult = seed();
    const auto firstResult = first.get();

    REQUIRE(firstResult.has_value() != secondResult.has_value());
    const auto& credentials = firstResult ? *firstResult : *secondResult;
    const UserService reopened(drogon::sync_wait(UserDao::init(openTestDatabase(path))));
    CHECK_FALSE(runOnEventLoop(reopened.seedDemo()));
    CHECK(authenticate(reopened, credentials.username, credentials.password));
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

TEST_CASE("API exception handling preserves field validation and hides internal failures", "[http]") {
    const auto request = drogon::HttpRequest::newHttpRequest();
    const auto response = apiException(*request, RequestValidationError(
        "The given data was invalid.", {{"email", {"Invalid email."}}}));
    CHECK(response->statusCode() == drogon::k400BadRequest);
    CHECK(response->getContentType() == drogon::CT_APPLICATION_JSON);
    CHECK(response->body().find(R"("email":["Invalid email."])") != std::string_view::npos);
    CHECK(response->body().find("VALIDATION_FAILED") != std::string_view::npos);

    const auto failure = apiException(*request, std::runtime_error("private failure"));
    CHECK(failure->statusCode() == drogon::k500InternalServerError);
    CHECK(failure->body().find("private failure") == std::string_view::npos);
    CHECK(failure->getHeader("X-Request-ID") == response->getHeader("X-Request-ID"));
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
