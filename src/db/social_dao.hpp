#pragma once

#include "user_dao.hpp"

namespace APP_NAMESPACE {

struct SocialPostFields {
    static constexpr std::string_view table_name = "social_posts";
    RequiredForeign<UserRecord> author_id;
    std::string text;
};
using SocialPost = DaoRecord<SocialPostFields>;

struct SocialCommentFields {
    static constexpr std::string_view table_name = "social_comments";
    Indexed<RequiredForeign<SocialPost>> post_id;
    RequiredForeign<UserRecord> author_id;
    std::string text;
};
using SocialComment = DaoRecord<SocialCommentFields>;

struct SocialLikeFields {
    static constexpr std::string_view table_name = "social_likes";
    RequiredForeign<SocialPost> post_id;
    RequiredForeign<UserRecord> user_id;
    static constexpr auto constraints = CompositeUnique<&SocialLikeFields::post_id, &SocialLikeFields::user_id>{};
};
using SocialLike = DaoRecord<SocialLikeFields>;

struct CommentView {
    std::int64_t id;
    std::string author;
    std::string text;
    std::string created_at;
};

struct LikeView {
    std::int64_t like_count;
    bool liked;
};

struct PostView {
    std::int64_t id;
    std::string author;
    std::string text;
    std::string created_at;
    std::int64_t comment_count;
    std::int64_t like_count;
    bool liked;
    std::optional<CommentView> latest_comment;
};

struct PostMatch {
    std::int64_t id;
    std::string author;
    std::string text;
};

class SocialPostDao final : public BaseDao<SocialPostDao, SocialPost> {
public:
    static drogon::Task<SocialPostDao> init(DatabaseBackendPtr database);
    drogon::Task<std::vector<PostView>> feed(std::int64_t user, std::int64_t limit,
                                            std::int64_t offset, std::optional<std::int64_t> author = {}) const &;
    drogon::Task<std::vector<PostView>> feed(std::int64_t user, std::int64_t limit,
                                            std::int64_t offset, std::optional<std::int64_t> author = {}) const && = delete;
    drogon::Task<std::optional<PostView>> view(std::int64_t user, std::int64_t post) const &;
    drogon::Task<std::optional<PostView>> view(std::int64_t user, std::int64_t post) const && = delete;
    drogon::Task<std::vector<PostMatch>> search(std::string query) const &;
    drogon::Task<std::vector<PostMatch>> search(std::string query) const && = delete;
private:
    drogon::Task<std::vector<PostView>> readViews(std::int64_t user, std::int64_t limit,
        std::int64_t offset, std::string filter, std::int64_t key) const;
    using BaseDao::BaseDao;
};

class SocialCommentDao final : public BaseDao<SocialCommentDao, SocialComment> {
public:
    static drogon::Task<SocialCommentDao> init(DatabaseBackendPtr database);
    drogon::Task<std::vector<CommentView>> thread(std::int64_t post, std::int64_t limit,
                                                 std::int64_t offset, const UserDao& users) const &;
    drogon::Task<std::vector<CommentView>> thread(std::int64_t post, std::int64_t limit,
                                                 std::int64_t offset, const UserDao& users) const && = delete;
    drogon::Task<std::vector<CommentView>> older(
        std::int64_t post, DaoDateTime beforeTime, std::int64_t beforeId,
        std::int64_t limit, const UserDao& users) const &;
    drogon::Task<std::vector<CommentView>> older(
        std::int64_t post, DaoDateTime beforeTime, std::int64_t beforeId,
        std::int64_t limit, const UserDao& users) const && = delete;
private:
    using BaseDao::BaseDao;
};

class SocialLikeDao final : public BaseDao<SocialLikeDao, SocialLike> {
public:
    static drogon::Task<SocialLikeDao> init(DatabaseBackendPtr database);
    drogon::Task<LikeView> set(std::int64_t post, std::int64_t user, bool liked) const &;
    drogon::Task<LikeView> set(std::int64_t post, std::int64_t user, bool liked) const && = delete;
private:
    using BaseDao::BaseDao;
};

}
