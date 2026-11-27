#include "social_dao.h"
#include "user_dao.h"

namespace APP_NAMESPACE {

drogon::Task<SocialPostDao> SocialPostDao::init(DatabaseBackendPtr database) {
    co_return co_await finish_initialization(SocialPostDao(std::move(database)));
}

drogon::Task<SocialCommentDao> SocialCommentDao::init(DatabaseBackendPtr database) {
    co_return co_await finish_initialization(SocialCommentDao(std::move(database)));
}

drogon::Task<SocialLikeDao> SocialLikeDao::init(DatabaseBackendPtr database) {
    co_return co_await finish_initialization(SocialLikeDao(std::move(database)));
}

drogon::Task<std::vector<PostView>> SocialPostDao::feed(
    std::int64_t user, std::int64_t limit, std::int64_t offset, std::optional<std::int64_t> author) const& {
    co_return co_await read_views(user, limit, offset, author ? " AND p.author_id = ?4" : "", author.value_or(0));
}

drogon::Task<std::optional<PostView>> SocialPostDao::view(std::int64_t user, std::int64_t post) const& {
    auto rows = co_await read_views(user, 1, 0, " AND p.id = ?4", post);
    if (rows.empty())
        co_return std::nullopt;
    co_return std::move(rows.front());
}

drogon::Task<std::vector<PostView>> SocialPostDao::read_views(
    std::int64_t user, std::int64_t limit, std::int64_t offset, std::string filter, std::int64_t key) const {
    const auto sql =
        "SELECT p.id, COALESCE(u.username, 'Deleted user'), p.text, p.created_at, "
        "(SELECT COUNT(*) FROM social_comments c WHERE c.post_id = p.id AND c.deleted_at IS NULL), "
        "(SELECT COUNT(*) FROM social_likes l WHERE l.post_id = p.id AND l.deleted_at IS NULL), "
        "EXISTS(SELECT 1 FROM social_likes l WHERE l.post_id = p.id AND l.user_id = ?1 AND l.deleted_at IS NULL), "
        "latest.id, COALESCE(reply_author.username, 'Deleted user'), latest.text, latest.created_at "
        "FROM social_posts p LEFT JOIN users u ON u.id = p.author_id AND u.deleted_at IS NULL "
        "LEFT JOIN social_comments latest ON latest.id = (SELECT c.id FROM social_comments c "
        "WHERE c.post_id = p.id AND c.deleted_at IS NULL ORDER BY c.created_at DESC, c.id DESC LIMIT 1) "
        "LEFT JOIN users reply_author ON reply_author.id = latest.author_id AND reply_author.deleted_at IS NULL "
        "WHERE p.deleted_at IS NULL" +
        filter +
        " ORDER BY p.created_at DESC, p.id DESC LIMIT ?2 OFFSET ?3";
    const auto rows = filter.empty() ? co_await execute_sql(sql, user, limit, offset)
                                     : co_await execute_sql(sql, user, limit, offset, key);
    std::vector<PostView> result;
    for (const auto& row : rows) {
        std::optional<CommentView> latest;
        if (!row[7].isNull()) {
            latest = CommentView{row[7].as<std::int64_t>(), row[8].as<std::string>(),
                                 row[9].as<std::string>(), row[10].as<std::string>()};
        }
        result.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>(),
                          row[2].as<std::string>(), row[3].as<std::string>(),
                          row[4].as<std::int64_t>(), row[5].as<std::int64_t>(),
                          row[6].as<int>() != 0, std::move(latest)});
    }
    co_return result;
}

drogon::Task<std::vector<PostMatch>> SocialPostDao::search(std::string query) const& {
    const auto rows = co_await execute_sql(
        "SELECT p.id, COALESCE(u.username, 'Deleted user'), p.text "
        "FROM social_posts p LEFT JOIN users u ON u.id = p.author_id AND u.deleted_at IS NULL "
        "WHERE p.deleted_at IS NULL AND instr(lower(p.text), lower(?1)) > 0 "
        "ORDER BY CASE WHEN lower(p.text) = lower(?1) THEN 0 "
        "WHEN instr(lower(p.text), lower(?1)) = 1 THEN 1 ELSE 2 END, "
        "p.created_at DESC, p.id DESC LIMIT 5",
        std::move(query));
    std::vector<PostMatch> result;
    for (const auto& row : rows) {
        result.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>(), row[2].as<std::string>()});
    }
    co_return result;
}

namespace {

std::vector<CommentView> comment_views(const std::vector<SocialComment>& rows) {
    std::vector<CommentView> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        const auto& author = row.author_id.record();
        result.push_back({row.id.value(), author ? author->username.value : "Deleted user",
                          row.text, detail::format_dao_date_time(row.created_at.value())});
    }
    return result;
}

}

drogon::Task<std::vector<CommentView>> SocialCommentDao::thread(
    std::int64_t post, std::int64_t limit, std::int64_t offset, const UserDao& users) const& {
    const auto rows = co_await find().limit(limit).offset(offset).select<&SocialComment::created_at>().where<&SocialComment::post_id>(post).order_by<&SocialComment::created_at>().order_by<&SocialComment::id>().with_foreign<&SocialComment::author_id>(users).collect();
    co_return comment_views(rows);
}

drogon::Task<std::vector<CommentView>> SocialCommentDao::older(
    std::int64_t post, DaoDateTime before_time, std::int64_t before_id,
    std::int64_t limit, const UserDao& users) const& {
    const auto rows = co_await find().limit(limit).select<&SocialComment::created_at>().where<&SocialComment::post_id>(post).where(where_tuple_less<&SocialComment::created_at, &SocialComment::id>(before_time, before_id)).order_by<&SocialComment::created_at>(SortDirection::desc).order_by<&SocialComment::id>(SortDirection::desc).with_foreign<&SocialComment::author_id>(users).collect();
    co_return comment_views(rows);
}

drogon::Task<LikeView> SocialLikeDao::set(std::int64_t post, std::int64_t user, bool liked) const& {
    LikeView result{};
    co_await transaction([&](DaoTransaction& tx) -> drogon::Task<> {
        const auto existing = co_await find(tx).limit(1).include_deleted().select<&SocialLike::deleted_at>().where<&SocialLike::post_id>(post).where<&SocialLike::user_id>(user).collect();
        if (liked && existing.empty()) {
            co_await create(tx, {{post, user}});
        } else if (liked && existing.front().deleted_at) {
            co_await restore_by_id(tx, existing.front().id.value());
        } else if (!liked && !existing.empty()) {
            co_await delete_by_id(tx, existing.front().id.value());
        }
        const auto total = co_await count(tx).where<&SocialLike::post_id>(post).collect();
        result = {total, liked};
    });
    co_return result;
}

}
