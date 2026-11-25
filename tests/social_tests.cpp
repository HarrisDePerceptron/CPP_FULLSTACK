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
