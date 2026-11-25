#include "social_dao.hpp"

namespace APP_NAMESPACE {

drogon::Task<SocialPostDao> SocialPostDao::init(DatabaseBackendPtr database) {
    co_return co_await finishInitialization(SocialPostDao(std::move(database)));
}

drogon::Task<SocialCommentDao> SocialCommentDao::init(DatabaseBackendPtr database) {
    co_return co_await finishInitialization(SocialCommentDao(std::move(database)));
}

drogon::Task<SocialLikeDao> SocialLikeDao::init(DatabaseBackendPtr database) {
    co_return co_await finishInitialization(SocialLikeDao(std::move(database)));
}

drogon::Task<std::vector<PostView>> SocialPostDao::feed(
    std::int64_t user, std::int64_t limit, std::int64_t offset) const {
    const auto rows = co_await executeSql(
        "SELECT p.id, COALESCE(u.username, 'Deleted user'), p.text, p.created_at, "
        "(SELECT COUNT(*) FROM social_comments c WHERE c.post_id = p.id AND c.deleted_at IS NULL), "
        "(SELECT COUNT(*) FROM social_likes l WHERE l.post_id = p.id AND l.deleted_at IS NULL), "
        "EXISTS(SELECT 1 FROM social_likes l WHERE l.post_id = p.id AND l.user_id = ? AND l.deleted_at IS NULL) "
        "FROM social_posts p LEFT JOIN users u ON u.id = p.author_id AND u.deleted_at IS NULL "
        "WHERE p.deleted_at IS NULL ORDER BY p.created_at DESC, p.id DESC LIMIT ? OFFSET ?", user, limit, offset);
    std::vector<PostView> result;
    for (const auto& row : rows) {
        result.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>(),
                          row[2].as<std::string>(), row[3].as<std::string>(),
                          row[4].as<std::int64_t>(), row[5].as<std::int64_t>(),
                          row[6].as<int>() != 0});
    }
    co_return result;
}

drogon::Task<std::vector<CommentView>> SocialCommentDao::thread(
    std::int64_t post, std::int64_t limit, std::int64_t offset, const UserDao& users) const {
    const auto rows = co_await find(limit, offset, select<&SocialComment::created_at>(),
        where<&SocialComment::post_id>(post),
        orderBy<&SocialComment::created_at>(), orderBy<&SocialComment::id>(),
        withForeign<&SocialComment::author_id>(users));
    std::vector<CommentView> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        const auto& author = row.author_id.record();
        result.push_back({row.id.value(), author ? author->username.value : "Deleted user",
                          row.text, detail::formatDaoDateTime(row.created_at.value())});
    }
    co_return result;
}

drogon::Task<LikeView> SocialLikeDao::set(std::int64_t post, std::int64_t user, bool liked) const {
    LikeView result{};
    co_await transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        const auto existing = co_await find(tx, 1, 0, includeDeleted(), select<&SocialLike::deleted_at>(),
            where<&SocialLike::post_id>(post), where<&SocialLike::user_id>(user));
        if (liked && existing.empty()) {
            co_await create(tx, {{post, user}});
        } else if (liked && existing.front().deleted_at) {
            co_await restoreById(tx, existing.front().id.value());
        } else if (!liked && !existing.empty()) {
            co_await deleteById(tx, existing.front().id.value());
        }
        const auto total = co_await count(tx, where<&SocialLike::post_id>(post));
        result = {total, liked};
    });
    co_return result;
}

}
