#pragma once

#include "app_namespace.h"

#include "account_models.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace APP_NAMESPACE {

struct SocialTextBody {
    std::string text;
};

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

struct SearchResults {
    std::vector<PostMatch> posts;
    std::vector<UserMatch> users;
};

struct CommentCursor {
    std::string created_at;
    std::int64_t id;
};

}
