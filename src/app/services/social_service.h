#pragma once

#include "app/dao/records/user_records.h"
#include "app/dao/social_dao.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace APP_NAMESPACE {

class UserService;

struct CommentCursor {
    std::string created_at;
    std::int64_t id;
};

struct SearchResults {
    std::vector<PostMatch> posts;
    std::vector<UserMatch> users;
};

class SocialService {
  public:
    static constexpr std::int64_t page_size = 20;
    static constexpr std::int64_t comment_batch_size = 5;

    SocialService(SocialPostDao posts, SocialCommentDao comments,
                  SocialLikeDao likes)
        : posts_(std::move(posts)), comments_(std::move(comments)),
          likes_(std::move(likes)) {}

    drogon::Task<std::vector<PostView>> feed(std::int64_t user,
                                             std::int64_t page) const;
    drogon::Task<SearchResults> search(std::string query,
                                       const UserService& users) const;
    drogon::Task<std::optional<PostView>> view(std::int64_t viewer,
                                               std::int64_t post) const;
    drogon::Task<std::optional<std::vector<PostView>>>
    user_posts(std::int64_t viewer, std::int64_t author, std::int64_t page,
               const UserService& users) const;
    drogon::Task<PostView> post(std::int64_t user, std::string username,
                                std::string text) const;
    drogon::Task<std::optional<std::vector<CommentView>>>
    thread(std::int64_t post, std::int64_t page,
           const UserService& users) const;
    drogon::Task<std::optional<std::vector<CommentView>>>
    older_comments(std::int64_t post, CommentCursor before,
                   const UserService& users) const;
    drogon::Task<std::optional<CommentView>> comment(std::int64_t post,
                                                     std::int64_t user,
                                                     std::string username,
                                                     std::string text) const;
    drogon::Task<std::optional<LikeView>>
    like(std::int64_t post, std::int64_t user, bool liked) const;

  private:
    SocialPostDao posts_;
    SocialCommentDao comments_;
    SocialLikeDao likes_;
};

}
