#pragma once

#include "app/models/social_models.h"
#include "app/models/social_records.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace APP_NAMESPACE {

class UserDao;

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
    drogon::Task<std::vector<PostView>> read_views(std::int64_t user, std::int64_t limit,
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
        std::int64_t post, DaoDateTime before_time, std::int64_t before_id,
        std::int64_t limit, const UserDao& users) const &;
    drogon::Task<std::vector<CommentView>> older(
        std::int64_t post, DaoDateTime before_time, std::int64_t before_id,
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
