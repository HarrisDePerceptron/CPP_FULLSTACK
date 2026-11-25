#include "services/social_service.hpp"
#include "services/user_service.hpp"
#include "utils/request_validation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>
#include <trantor/net/EventLoopThread.h>

namespace APP_NAMESPACE {
namespace {

template <typename Work>
void onDatabaseLoop(Work work) {
    trantor::EventLoopThread thread;
    thread.run();
    const auto run = [&]() -> drogon::Task<> {
        co_await drogon::switchThreadCoro(thread.getLoop());
        co_await work();
    };
    drogon::sync_wait(run());
}

struct SocialFixture {
    UserService users;
    SocialService social;

    static drogon::Task<SocialFixture> create() {
        auto database = co_await openSqliteDatabase(":memory:");
        auto users = co_await UserDao::init(database);
        auto posts = co_await SocialPostDao::init(database);
        auto comments = co_await SocialCommentDao::init(database);
        auto likes = co_await SocialLikeDao::init(database);
        co_return SocialFixture{UserService(std::move(users)),
            SocialService(std::move(posts), std::move(comments), std::move(likes))};
    }

    drogon::Task<UserRecord::Id> account(std::string name) {
        co_return co_await users.dao().create({{std::move(name), "hash", std::nullopt}});
    }
};

TEST_CASE("Signup stores a usable password and rejects a duplicate username", "[social]") {
    REQUIRE(sodium_init() >= 0);
    onDatabaseLoop([]() -> drogon::Task<> {
        auto database = co_await openSqliteDatabase(":memory:");
        UserService users(co_await UserDao::init(database));
        Workers workers;
        const Credentials credentials{"alice", "example-password"};
        CHECK(co_await users.signup(credentials, workers));
        CHECK_FALSE(co_await users.signup(credentials, workers));
        CHECK(co_await users.authenticate(credentials.username, credentials.password, workers));
    });
}

TEST_CASE("The feed attributes posts and comments to their respective users", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto fixture = co_await SocialFixture::create();
        const auto alice = co_await fixture.account("alice");
        const auto bob = co_await fixture.account("bob");
        const auto post = co_await fixture.social.post(alice, "alice", "  Hello  ");
        REQUIRE(co_await fixture.social.comment(post.id, bob, "bob", "Reply"));
        const auto feed = co_await fixture.social.feed(bob, 1);
        REQUIRE(feed.size() == 1);
        CHECK(feed.front().author == "alice");
        CHECK(feed.front().text == "Hello");
        CHECK(feed.front().comment_count == 1);
        const auto thread = co_await fixture.social.thread(post.id, 1, fixture.users);
        REQUIRE(thread);
        REQUIRE(thread->size() == 1);
        CHECK(thread->front().author == "bob");
    });
}

TEST_CASE("Like state is per user and repeated writes are idempotent", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto fixture = co_await SocialFixture::create();
        const auto alice = co_await fixture.account("alice");
        const auto bob = co_await fixture.account("bob");
        const auto post = co_await fixture.social.post(alice, "alice", "Hello");
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto liked = co_await fixture.social.like(post.id, bob, true);
            REQUIRE(liked);
            CHECK(liked->liked);
            CHECK(liked->like_count == 1);
        }
        const auto feed = co_await fixture.social.feed(alice, 1);
        CHECK_FALSE(feed.at(0).liked);
        CHECK(feed.at(0).like_count == 1);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto cleared = co_await fixture.social.like(post.id, bob, false);
            REQUIRE(cleared);
            CHECK_FALSE(cleared->liked);
            CHECK(cleared->like_count == 0);
        }
    });
}

TEST_CASE("The feed hides deleted content and a new like restores a deleted like", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto database = co_await openSqliteDatabase(":memory:");
        const auto users = co_await UserDao::init(database);
        const auto posts = co_await SocialPostDao::init(database);
        const auto comments = co_await SocialCommentDao::init(database);
        const auto likes = co_await SocialLikeDao::init(database);
        const auto alice = co_await users.create({{"alice", "hash"}});
        const auto bob = co_await users.create({{"bob", "hash"}});
        const auto post = co_await posts.create({{alice, "Hello"}});
        const auto hidden = co_await posts.create({{alice, "Hidden"}});
        const auto reply = co_await comments.create({{post, alice, "Reply"}});
        co_await comments.create({{post, alice, "Visible"}});
        const auto like = co_await likes.create({{post, bob}});
        CHECK(co_await users.softDeleteById(alice));
        CHECK(co_await posts.softDeleteById(hidden));
        CHECK(co_await comments.softDeleteById(reply));
        CHECK(co_await likes.softDeleteById(like));

        const auto feed = co_await posts.feed(bob, 20, 0);
        REQUIRE(feed.size() == 1);
        CHECK(feed.front().author == "Deleted user");
        CHECK(feed.front().comment_count == 1);
        CHECK(feed.front().like_count == 0);
        CHECK_FALSE(feed.front().liked);
        const auto thread = co_await comments.thread(post, 20, 0, users);
        REQUIRE(thread.size() == 1);
        CHECK(thread.front().author == "Deleted user");
        const auto restored = co_await likes.set(post, bob, true);
        CHECK(restored.liked);
        CHECK(restored.like_count == 1);
        CHECK(co_await likes.findById(like));
    });
}

TEST_CASE("Search ranks literal active post and username matches independently", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto database = co_await openSqliteDatabase(":memory:");
        UserService users(co_await UserDao::init(database));
        auto posts = co_await SocialPostDao::init(database);
        auto comments = co_await SocialCommentDao::init(database);
        auto likes = co_await SocialLikeDao::init(database);
        auto& accounts = users.dao();
        const auto exactUser = co_await accounts.create({{"A_%", "hash"}});
        for (int i = 0; i < 6; ++i) {
            co_await accounts.create({{"a_%" + std::to_string(i), "hash"}});
        }
        const auto deletedUser = co_await accounts.create({{"a_%deleted", "hash"}});
        REQUIRE(co_await accounts.softDeleteById(deletedUser));
        const auto author = co_await accounts.create({{"author", "hash"}});
        const auto exactPost = co_await posts.create({{author, "A_%"}});
        const auto prefixPost = co_await posts.create({{author, "a_% prefix"}});
        const auto deletedPost = co_await posts.create({{author, "a_%"}});
        REQUIRE(co_await posts.softDeleteById(deletedPost));
        const auto unrelated = co_await posts.create({{author, "unrelated"}});
        co_await comments.create({{unrelated, author, "a_%"}});

        SocialService social(std::move(posts), std::move(comments), std::move(likes));
        const auto initial = co_await social.search(" a_% ", users);
        REQUIRE(initial.posts.size() == 2);
        REQUIRE(initial.users.size() == 5);
        CHECK(initial.users.front().id == exactUser);
        CHECK(initial.users[1].username == "a_%0");
        CHECK(initial.posts[0].id == exactPost);
        CHECK(initial.posts[1].id == prefixPost);
        CHECK_FALSE(co_await social.view(author, deletedPost));
        CHECK_FALSE(co_await social.userPosts(author, deletedUser, 1, users));
        const auto userPosts = co_await social.userPosts(author, exactUser, 1, users);
        REQUIRE(userPosts);
        CHECK(userPosts->empty());
        for (int i = 0; i < 6; ++i) {
            co_await social.post(author, "author", "suffix a_% " + std::to_string(i));
        }
        const auto capped = co_await social.search("a_%", users);
        REQUIRE(capped.posts.size() == 5);
        CHECK(capped.posts[0].id == exactPost);
        CHECK(capped.posts[1].id == prefixPost);
        CHECK(capped.posts[2].text == "suffix a_% 5");
        CHECK((co_await social.search("  ", users)).posts.empty());
    });
}

TEST_CASE("Feed previews the newest visible reply with ID as a timestamp tie breaker", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto database = co_await openSqliteDatabase(":memory:");
        const auto users = co_await UserDao::init(database);
        const auto posts = co_await SocialPostDao::init(database);
        const auto comments = co_await SocialCommentDao::init(database);
        co_await SocialLikeDao::init(database);
        const auto author = co_await users.create({{"author", "hash"}});
        const auto post = co_await posts.create({{author, "parent"}});
        const auto empty = co_await posts.create({{author, "empty"}});
        CHECK_FALSE((co_await posts.view(author, empty))->latest_comment);
        co_await comments.create({{post, author, "first"}});
        const auto second = co_await comments.create({{post, author, "second"}});
        const auto hidden = co_await comments.create({{post, author, "hidden"}});
        REQUIRE(co_await comments.softDeleteById(hidden));
        co_await comments.executeSql("UPDATE social_comments SET created_at = '2026-11-25T01:00:00.000Z'");
        const auto view = co_await posts.view(author, post);
        REQUIRE(view);
        REQUIRE(view->latest_comment);
        CHECK(view->latest_comment->id == second);
        CHECK(view->comment_count == 2);
    });
}

TEST_CASE("Older comment batches skip deleted rows and stay stable as replies arrive", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto fixture = co_await SocialFixture::create();
        const auto author = co_await fixture.account("alice");
        const auto post = co_await fixture.social.post(author, "alice", "Parent");
        const auto stamp = "2026-11-25T01:00:00.000Z";
        const auto empty = co_await fixture.social.olderComments(post.id, {stamp, 1}, fixture.users);
        REQUIRE(empty);
        CHECK(empty->empty());

        std::vector<std::int64_t> ids;
        for (int i = 0; i < 13; ++i) {
            const auto reply = co_await fixture.social.comment(post.id, author, "alice", "Comment");
            REQUIRE(reply);
            if (i == 0) {
                const auto noOlder = co_await fixture.social.olderComments(post.id, {reply->created_at, reply->id}, fixture.users);
                REQUIRE(noOlder);
                CHECK(noOlder->empty());
            }
            ids.push_back(reply->id);
        }
        const auto& dao = fixture.users.dao();
        co_await dao.executeSql("UPDATE social_comments SET created_at = ? WHERE post_id = ?", stamp, post.id);
        co_await dao.executeSql("UPDATE social_comments SET deleted_at = ? WHERE id = ?", stamp, ids[9]);
        const auto first = co_await fixture.social.olderComments(post.id, {stamp, ids.back()}, fixture.users);
        REQUIRE(first);
        REQUIRE(first->size() == 6);
        CHECK((*first)[0].id == ids[11]);
        CHECK((*first)[1].id == ids[10]);
        CHECK((*first)[2].id == ids[8]);
        CHECK((*first)[4].id == ids[6]);

        REQUIRE(co_await fixture.social.comment(post.id, author, "alice", "New reply"));
        const auto second = co_await fixture.social.olderComments(post.id, {stamp, (*first)[4].id}, fixture.users);
        REQUIRE(second);
        REQUIRE(second->size() == 6);
        CHECK((*second)[0].id == ids[5]);
        CHECK((*second)[4].id == ids[1]);
        const auto tail = co_await fixture.social.olderComments(post.id, {stamp, (*second)[4].id}, fixture.users);
        REQUIRE(tail);
        REQUIRE(tail->size() == 1);
        CHECK(tail->front().id == ids.front());
    });
}

TEST_CASE("Feed pages are ordered newest first with one lookahead row", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto fixture = co_await SocialFixture::create();
        const auto alice = co_await fixture.account("alice");
        const auto first = co_await fixture.social.post(alice, "alice", "First");
        for (int index = 0; index < SocialService::pageSize; ++index) {
            co_await fixture.social.post(alice, "alice", "Next");
        }
        const auto page = co_await fixture.social.feed(alice, 1);
        REQUIRE(page.size() == SocialService::pageSize + 1);
        CHECK(page.back().id == first.id);
        const auto next = co_await fixture.social.feed(alice, 2);
        REQUIRE(next.size() == 1);
        CHECK(next.front().id == first.id);
    });
}

TEST_CASE("The social service rejects blank or oversized posts", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto fixture = co_await SocialFixture::create();
        const auto alice = co_await fixture.account("alice");
        const auto reject = [&](std::string text) -> drogon::Task<bool> {
            try {
                co_await fixture.social.post(alice, "alice", std::move(text));
            } catch (const RequestValidationError&) {
                co_return true;
            }
            co_return false;
        };
        CHECK(co_await reject(" \n\t "));
        CHECK(co_await reject(std::string(281, 'a')));
    });
}

TEST_CASE("Comment and like operations report a missing post", "[social]") {
    onDatabaseLoop([]() -> drogon::Task<> {
        auto fixture = co_await SocialFixture::create();
        const auto alice = co_await fixture.account("alice");
        CHECK_FALSE(co_await fixture.social.thread(42, 1, fixture.users));
        CHECK_FALSE(co_await fixture.social.comment(42, alice, "alice", "Reply"));
        CHECK_FALSE(co_await fixture.social.like(42, alice, true));
    });
}

}
}
