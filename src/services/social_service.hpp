#pragma once

#include "db/social_dao.hpp"

namespace APP_NAMESPACE {

class UserService;

class SocialService {
public:
    static constexpr std::int64_t pageSize = 20;

    SocialService(SocialPostDao posts, SocialCommentDao comments, SocialLikeDao likes)
        : posts_(std::move(posts)), comments_(std::move(comments)),
          likes_(std::move(likes)) {}

    drogon::Task<std::vector<PostView>> feed(std::int64_t user, std::int64_t page) const;
    drogon::Task<PostView> post(std::int64_t user, std::string username, std::string text) const;
    drogon::Task<std::optional<std::vector<CommentView>>> thread(
        std::int64_t post, std::int64_t page, const UserService& users) const;
    drogon::Task<std::optional<CommentView>> comment(std::int64_t post, std::int64_t user,
                                                     std::string username, std::string text) const;
    drogon::Task<std::optional<LikeView>> like(std::int64_t post, std::int64_t user, bool liked) const;

private:
    SocialPostDao posts_;
    SocialCommentDao comments_;
    SocialLikeDao likes_;
};

}
