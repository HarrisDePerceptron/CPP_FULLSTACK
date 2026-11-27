#include "social_service.h"
#include "user_service.h"
#include "framework/http/request_validation.h"

#include <simdjson.h>

namespace APP_NAMESPACE {
namespace {

std::string validate_text(std::string text, std::size_t limit) {
    constexpr auto whitespace = " \r\n\t\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first != std::string::npos && text.find('\0') == std::string::npos &&
        simdjson::validate_utf8(text)) {
        text =
            text.substr(first, text.find_last_not_of(whitespace) - first + 1);
        std::size_t length = 0;
        for (unsigned char byte : text) {
            if ((byte & 0xc0) != 0x80)
                ++length;
        }
        if (length <= limit)
            return text;
    }
    throw RequestValidationError("Enter text within the character limit.");
}

}

drogon::Task<std::vector<PostView>>
SocialService::feed(std::int64_t user, std::int64_t page) const {
    co_return co_await posts_.feed(user, page_size + 1, (page - 1) * page_size);
}

drogon::Task<SearchResults>
SocialService::search(std::string query, const UserService& users) const {
    if (query.find_first_not_of(" \r\n\t\f\v") == std::string::npos)
        co_return SearchResults{};
    query = validate_text(std::move(query), 128);
    auto posts = co_await posts_.search(query);
    auto matches = co_await users.dao().search(std::move(query));
    co_return SearchResults{std::move(posts), std::move(matches)};
}

drogon::Task<std::optional<PostView>>
SocialService::view(std::int64_t viewer, std::int64_t post) const {
    co_return co_await posts_.view(viewer, post);
}

drogon::Task<std::optional<std::vector<PostView>>>
SocialService::user_posts(std::int64_t viewer, std::int64_t author,
                          std::int64_t page, const UserService& users) const {
    if (!co_await users.dao().find_by_id(author).collect())
        co_return std::nullopt;
    co_return co_await posts_.feed(viewer, page_size + 1,
                                   (page - 1) * page_size, author);
}

drogon::Task<PostView> SocialService::post(std::int64_t user,
                                           std::string username,
                                           std::string text) const {
    text = validate_text(std::move(text), 280);
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

drogon::Task<std::optional<std::vector<CommentView>>>
SocialService::thread(std::int64_t post, std::int64_t page,
                      const UserService& users) const {
    if (!co_await posts_.find_by_id(post).collect())
        co_return std::nullopt;
    co_return co_await comments_.thread(post, page_size + 1,
                                        (page - 1) * page_size, users.dao());
}

drogon::Task<std::optional<std::vector<CommentView>>>
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
    co_return co_await comments_.older(post, before_time, before.id,
                                       comment_batch_size + 1, users.dao());
}

drogon::Task<std::optional<CommentView>>
SocialService::comment(std::int64_t post, std::int64_t user,
                       std::string username, std::string text) const {
    text = validate_text(std::move(text), 500);
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
