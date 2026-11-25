#include "routes.hpp"
#include "server/auth/jwt_middleware.hpp"
#include "server/json_response.hpp"
#include "server/services.hpp"

#include <drogon/drogon.h>

#include <charconv>
#include <limits>

namespace APP_NAMESPACE {

struct SocialTextBody {
    std::string text;
};

namespace {

using namespace drogon;

std::int64_t positiveInteger(std::string_view value) {
    std::int64_t number{};
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < 1) {
        throw RequestValidationError("Use a positive integer for the post ID or page.");
    }
    return number;
}

std::int64_t pageNumber(const HttpRequest& request) {
    const auto& value = request.getParameter("page");
    const auto page = value.empty() ? 1 : positiveInteger(value);
    if (page > std::numeric_limits<std::int64_t>::max() / SocialService::pageSize) {
        throw RequestValidationError("Page is out of range.");
    }
    return page;
}

std::string readText(const HttpRequest& request) {
    SocialTextBody payload;
    const auto body = request.body();
    if (request.contentType() != CT_APPLICATION_JSON || body.size() > 8192 ||
        glz::validate_json(body) || glz::read<glz::opts{.error_on_missing_keys = true}>(payload, body)) {
        throw RequestValidationError("Send a JSON text field.");
    }
    return std::move(payload.text);
}

template <typename Item>
HttpResponsePtr pageResponse(const HttpRequest& request, std::vector<Item> items, std::int64_t page,
                             std::int64_t perPage = SocialService::pageSize) {
    const bool hasMore = items.size() > static_cast<std::size_t>(perPage);
    if (hasMore) items.pop_back();
    return apiOk(request, std::move(items), ApiPagination{page, perPage, hasMore});
}

HttpResponsePtr missingPost(const HttpRequest& request) {
    return apiError(request, k404NotFound, "NOT_FOUND", "That post could not be found.");
}

HttpResponsePtr unauthorized(const HttpRequest& request) {
    return apiError(request, k401Unauthorized, "UNAUTHORIZED", "Sign in to use the social feed.");
}

HttpResponsePtr noStore(HttpResponsePtr response) {
    response->addHeader("Cache-Control", "no-store");
    return response;
}

}

void registerSocialRoutes(const std::shared_ptr<Services>& services, const std::string& jwt) {
    auto& server = app();
    server.registerHandler("/", [services](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        const auto username = services->tokens.verify(request->getCookie(sessionCookieName));
        if (!username || !co_await services->users.findId(*username)) {
            co_return noStore(HttpResponse::newRedirectionResponse("/social/login"));
        }
        co_return noStore(HttpResponse::newHttpViewResponse("AppViews::social"));
    }, {Get});
    server.registerHandler("/social", [](HttpRequestPtr) -> Task<HttpResponsePtr> {
        co_return noStore(HttpResponse::newRedirectionResponse("/"));
    }, {Get});
    server.registerHandler("/social/login", [](HttpRequestPtr) -> Task<HttpResponsePtr> {
        co_return noStore(HttpResponse::newHttpViewResponse("AppViews::social_login"));
    }, {Get});
    server.registerHandler("/api/search", [services](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        const auto viewer = co_await services->users.findId(request->attributes()->get<std::string>(subjectAttribute));
        if (!viewer) co_return unauthorized(*request);
        auto matches = co_await services->social.search(request->getParameter("q"), services->users);
        co_return noStore(apiOk(*request, std::move(matches)));
    }, {Get, jwt});
    server.registerHandler("/api/posts/{1}",
        [services](HttpRequestPtr request, std::string id) -> Task<HttpResponsePtr> {
            const auto viewer = co_await services->users.findId(request->attributes()->get<std::string>(subjectAttribute));
            if (!viewer) co_return unauthorized(*request);
            auto post = co_await services->social.view(*viewer, positiveInteger(id));
            if (!post) co_return missingPost(*request);
            co_return noStore(apiOk(*request, std::move(*post)));
        }, {Get, jwt});
    server.registerHandler("/api/users/{1}/posts",
        [services](HttpRequestPtr request, std::string id) -> Task<HttpResponsePtr> {
            const auto viewer = co_await services->users.findId(request->attributes()->get<std::string>(subjectAttribute));
            if (!viewer) co_return unauthorized(*request);
            const auto page = pageNumber(*request);
            auto profile = co_await services->social.userPosts(*viewer, positiveInteger(id), page, services->users);
            if (!profile) co_return apiError(*request, k404NotFound, "NOT_FOUND", "That person could not be found.");
            co_return noStore(pageResponse(*request, std::move(*profile), page));
        }, {Get, jwt});
    server.registerHandler("/api/posts", [services](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        const auto username = request->attributes()->get<std::string>(subjectAttribute);
        const auto user = co_await services->users.findId(username);
        if (!user) co_return unauthorized(*request);
        if (request->method() == Post) {
            auto post = co_await services->social.post(*user, username, readText(*request));
            co_return noStore(apiOk(*request, std::move(post), k201Created));
        }
        const auto page = pageNumber(*request);
        auto posts = co_await services->social.feed(*user, page);
        co_return noStore(pageResponse(*request, std::move(posts), page));
    }, {Get, Post, jwt});
    server.registerHandler("/api/posts/{1}/comments",
        [services](HttpRequestPtr request, std::string id) -> Task<HttpResponsePtr> {
            const auto username = request->attributes()->get<std::string>(subjectAttribute);
            const auto user = co_await services->users.findId(username);
            if (!user) co_return unauthorized(*request);
            const auto post = positiveInteger(id);
            if (request->method() == Post) {
                auto comment = co_await services->social.comment(post, *user, username, readText(*request));
                if (!comment) co_return missingPost(*request);
                co_return noStore(apiOk(*request, std::move(*comment), k201Created));
            }
            const auto& parameters = request->getParameters();
            if (parameters.contains("before_id") || parameters.contains("before_time")) {
                const auto& time = request->getParameter("before_time");
                const auto& id = request->getParameter("before_id");
                if (time.empty() || id.empty() || parameters.contains("page")) {
                    throw RequestValidationError("Send before_time and before_id together without page.");
                }
                auto comments = co_await services->social.olderComments(
                    post, CommentCursor{time, positiveInteger(id)}, services->users);
                if (!comments) co_return missingPost(*request);
                co_return noStore(pageResponse(*request, std::move(*comments), 1, SocialService::commentBatchSize));
            }
            const auto page = pageNumber(*request);
            auto comments = co_await services->social.thread(post, page, services->users);
            if (!comments) co_return missingPost(*request);
            co_return noStore(pageResponse(*request, std::move(*comments), page));
        }, {Get, Post, jwt});
    server.registerHandler("/api/posts/{1}/like",
        [services](HttpRequestPtr request, std::string id) -> Task<HttpResponsePtr> {
            const auto user = co_await services->users.findId(
                request->attributes()->get<std::string>(subjectAttribute));
            if (!user) co_return unauthorized(*request);
            auto like = co_await services->social.like(positiveInteger(id), *user, request->method() == Put);
            if (!like) co_return missingPost(*request);
            co_return noStore(apiOk(*request, *like));
        }, {Put, Delete, jwt});
}

}
