#pragma once

#include "app_namespace.h"

#include "account_records.h"
#include "framework/db/base_dao.h"

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
    static constexpr auto constraints = CompositeUnique<&SocialLikeFields::post_id, &SocialLikeFields::user_id>{};
};
using SocialLike = DaoRecord<SocialLikeFields>;

}
