#include "social_service.h"
#include "user_service.h"
#include "framework/http/request_validation.h"

namespace APP_NAMESPACE {

drogon::Task<PagedResult<PostView>> SocialService::feed(std::int64_t user,
                                                        PageQuery page) const {
    co_return co_await posts_.feed(user, page);
}

drogon::Task<SearchResults>
SocialService::search(std::string query, const UserService& users) const {
    if (query.empty())
        co_return SearchResults{};
    auto posts = co_await posts_.search(query);
    auto matches = co_await users.dao().search(std::move(query));
    co_return SearchResults{std::move(posts), std::move(matches)};
}

drogon::Task<std::optional<PostView>>
SocialService::view(std::int64_t viewer, std::int64_t post) const {
    co_return co_await posts_.view(viewer, post);
}

drogon::Task<std::optional<PagedResult<PostView>>>
SocialService::user_posts(std::int64_t viewer, std::int64_t author,
                          PageQuery page, const UserService& users) const {
    if (!co_await users.dao().find_by_id(author).collect())
        co_return std::nullopt;
    co_return co_await posts_.feed(viewer, page, author);
}

drogon::Task<PostView> SocialService::post(std::int64_t user,
                                           std::string username,
                                           std::string text) const {
    const auto id = co_await posts_.create({{user, text}});
    const auto row = co_await posts_.find_by_id(id).collect();
    co_return PostView{id,
                       std::move(username),
                       std::move(text),
                       detail::format_dao_date_time(row->created_at.value()),
                       0,
                       0,
                       false,
                       {}};
}

drogon::Task<std::optional<PagedResult<CommentView>>>
SocialService::thread(std::int64_t post, PageQuery page,
                      const UserService& users) const {
    if (!co_await posts_.find_by_id(post).collect())
        co_return std::nullopt;
    co_return co_await comments_.thread(post, page, users.dao());
}

drogon::Task<std::optional<PagedResult<CommentView>>>
SocialService::older_comments(std::int64_t post, CommentCursor before,
                              const UserService& users) const {
    if (!co_await posts_.find_by_id(post).collect())
        co_return std::nullopt;
    DaoDateTime before_time;
    try {
        before_time = detail::parse_dao_date_time(before.created_at);
    } catch (const std::runtime_error&) {
        throw RequestValidationError("Use a valid UTC comment timestamp.");
    }
    co_return co_await comments_.older(
        post, before_time, before.id,
        PageQuery::numbered(1, comment_batch_size), users.dao());
}

drogon::Task<std::optional<CommentView>>
SocialService::comment(std::int64_t post, std::int64_t user,
                       std::string username, std::string text) const {
    if (!co_await posts_.find_by_id(post).collect())
        co_return std::nullopt;
    const auto id = co_await comments_.create({{post, user, text}});
    const auto row = co_await comments_.find_by_id(id).collect();
    co_return CommentView{
        id, std::move(username), std::move(text),
        detail::format_dao_date_time(row->created_at.value())};
}

drogon::Task<std::optional<LikeView>>
SocialService::like(std::int64_t post, std::int64_t user, bool liked) const {
    if (!co_await posts_.find_by_id(post).collect())
        co_return std::nullopt;
    co_return co_await likes_.set(post, user, liked);
}

}
