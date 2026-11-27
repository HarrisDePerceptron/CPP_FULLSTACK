#pragma once

#include "app_namespace.h"

#include "user_records.h"
#include "framework/db/base_dao.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
    static constexpr auto constraints =
        CompositeUnique<&SocialLikeFields::post_id,
                        &SocialLikeFields::user_id>{};
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

}
