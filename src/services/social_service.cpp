#include "social_service.hpp"
#include "user_service.hpp"
#include "utils/request_validation.hpp"

#include <simdjson.h>

namespace APP_NAMESPACE {
namespace {

std::string validateText(std::string text, std::size_t limit) {
    constexpr auto whitespace = " \r\n\t\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first != std::string::npos && text.find('\0') == std::string::npos && simdjson::validate_utf8(text)) {
        text = text.substr(first, text.find_last_not_of(whitespace) - first + 1);
        std::size_t length = 0;
        for (unsigned char byte : text) {
            if ((byte & 0xc0) != 0x80) ++length;
        }
        if (length <= limit) return text;
    }
    throw RequestValidationError("Enter text within the character limit.");
}

}

drogon::Task<std::vector<PostView>> SocialService::feed(std::int64_t user, std::int64_t page) const {
    co_return co_await posts_.feed(user, pageSize + 1, (page - 1) * pageSize);
}

drogon::Task<PostView> SocialService::post(std::int64_t user, std::string username, std::string text) const {
    text = validateText(std::move(text), 280);
    const auto id = co_await posts_.create({{user, text}});
    const auto row = co_await posts_.findById(id);
    co_return PostView{id, std::move(username), std::move(text),
                       detail::formatDaoDateTime(row->created_at.value()), 0, 0, false};
}

drogon::Task<std::optional<std::vector<CommentView>>> SocialService::thread(
    std::int64_t post, std::int64_t page, const UserService& users) const {
    if (!co_await posts_.findById(post)) co_return std::nullopt;
    co_return co_await comments_.thread(post, pageSize + 1, (page - 1) * pageSize, users.dao());
}

drogon::Task<std::optional<CommentView>> SocialService::comment(
    std::int64_t post, std::int64_t user, std::string username, std::string text) const {
    text = validateText(std::move(text), 500);
    if (!co_await posts_.findById(post)) co_return std::nullopt;
    const auto id = co_await comments_.create({{post, user, text}});
    const auto row = co_await comments_.findById(id);
    co_return CommentView{id, std::move(username), std::move(text),
                          detail::formatDaoDateTime(row->created_at.value())};
}

drogon::Task<std::optional<LikeView>> SocialService::like(
    std::int64_t post, std::int64_t user, bool liked) const {
    if (!co_await posts_.findById(post)) co_return std::nullopt;
    co_return co_await likes_.set(post, user, liked);
}

}
