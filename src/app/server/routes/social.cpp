#include "app/server/dto/social_dtos.h"
#include "app/server/routes.h"
#include "framework/auth/auth.h"
#include "framework/http/json_response.h"
#include "framework/http/pagination.h"
#include "framework/support/validation.h"
#include "app/server/services.h"

#include <drogon/drogon.h>

#include <charconv>

namespace APP_NAMESPACE {

namespace {

using namespace drogon;

std::int64_t positive_integer(std::string_view value) {
    std::int64_t number{};
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < 1) {
        throw RequestValidationError(
            "Use a positive integer for the post ID or page.");
    }
    return number;
}

std::string normalize_text(std::string_view text, std::size_t limit,
                           bool allow_blank = false) {
    text = trim_ascii_whitespace(text);
    const auto length = utf8_code_point_count(text);
    if ((!allow_blank && text.empty()) || !length || *length > limit ||
        text.contains('\0')) {
        throw RequestValidationError("Enter text within the character limit.");
    }
    return std::string(text);
}

std::string read_text(const HttpRequest& request, std::size_t limit) {
    SocialTextBody payload;
    const auto body = request.body();
    if (request.contentType() != CT_APPLICATION_JSON || body.size() > 8192 ||
        glz::validate_json(body) ||
        glz::read<glz::opts{.error_on_missing_keys = true}>(payload, body)) {
        throw RequestValidationError("Send a JSON text field.");
    }
    return normalize_text(payload.text, limit);
}

HttpResponsePtr missing_post(const HttpRequest& request) {
    return api_error(request, k404NotFound, "NOT_FOUND",
                     "That post could not be found.");
}

HttpResponsePtr unauthorized(const HttpRequest& request) {
    return api_error(request, k401Unauthorized, "UNAUTHORIZED",
                     "Sign in to use the social feed.");
}

HttpResponsePtr no_store(HttpResponsePtr response) {
    response->addHeader("Cache-Control", "no-store");
    return response;
}

Task<HttpResponsePtr> uncached_list(Task<HttpResponsePtr> pending) {
    auto response = co_await std::move(pending);
    if (response->statusCode() == k200OK)
        co_return no_store(std::move(response));
    co_return response;
}

HttpResponsePtr render_page(const std::string& view,
                            const std::string& public_url_prefix) {
    HttpViewData data;
    data.insert("public_url_prefix", public_url_prefix);
    return no_store(HttpResponse::newHttpViewResponse(view, data));
}

Task<HttpResponsePtr> feed_page(std::shared_ptr<Services> services,
                                HttpRequestPtr request,
                                const std::string& public_url_prefix) {
    const auto token =
        services->tokens.verify(request->getCookie(session_cookie_name));
    if (!token || !co_await services->users.find_id(token->subject)) {
        co_return no_store(
            HttpResponse::newRedirectionResponse("/social/login"));
    }
    co_return render_page("AppViews::social", public_url_prefix);
}

Task<HttpResponsePtr> redirect_to_feed(HttpRequestPtr) {
    co_return no_store(HttpResponse::newRedirectionResponse("/"));
}

Task<HttpResponsePtr> login_page(const std::string& public_url_prefix) {
    co_return render_page("AppViews::social_login", public_url_prefix);
}

Task<HttpResponsePtr> search(std::shared_ptr<Services> services,
                             HttpRequestPtr request) {
    const auto viewer =
        co_await services->users.find_id(get_current_username(*request));
    if (!viewer)
        co_return unauthorized(*request);
    auto matches = co_await services->social.search(
        normalize_text(request->getParameter("q"), 128, true), services->users);
    co_return no_store(api_ok(*request, std::move(matches)));
}

Task<HttpResponsePtr> view_post(std::shared_ptr<Services> services,
                                HttpRequestPtr request, std::string id) {
    const auto viewer =
        co_await services->users.find_id(get_current_username(*request));
    if (!viewer)
        co_return unauthorized(*request);
    auto post = co_await services->social.view(*viewer, positive_integer(id));
    if (!post)
        co_return missing_post(*request);
    co_return no_store(api_ok(*request, std::move(*post)));
}

Task<HttpResponsePtr> list_user_posts(std::shared_ptr<Services> services,
                                      HttpRequestPtr request, std::string id) {
    const auto viewer =
        co_await services->users.find_id(get_current_username(*request));
    if (!viewer)
        co_return unauthorized(*request);
    const auto page = read_page(*request);
    auto profile = co_await services->social.user_posts(
        *viewer, positive_integer(id), page, services->users);
    if (!profile)
        co_return api_error(*request, k404NotFound, "NOT_FOUND",
                            "That person could not be found.");
    co_return api_ok(*request, std::move(*profile));
}

Task<HttpResponsePtr> list_posts(std::shared_ptr<Services> services,
                                 HttpRequestPtr request) {
    const auto user =
        co_await services->users.find_id(get_current_username(*request));
    if (!user)
        co_return unauthorized(*request);
    auto posts = co_await services->social.feed(*user, read_page(*request));
    co_return api_ok(*request, std::move(posts));
}

Task<HttpResponsePtr> create_post(std::shared_ptr<Services> services,
                                  HttpRequestPtr request) {
    const auto username = get_current_username(*request);
    const auto user = co_await services->users.find_id(username);
    if (!user)
        co_return unauthorized(*request);
    auto post = co_await services->social.post(*user, username,
                                               read_text(*request, 280));
    co_return no_store(api_ok(*request, std::move(post), k201Created));
}

Task<HttpResponsePtr> list_comments(std::shared_ptr<Services> services,
                                    HttpRequestPtr request, std::string id) {
    const auto user =
        co_await services->users.find_id(get_current_username(*request));
    if (!user)
        co_return unauthorized(*request);
    const auto post = positive_integer(id);
    const auto& parameters = request->getParameters();
    std::optional<PagedResult<CommentView>> comments;
    if (parameters.contains("before_id") ||
        parameters.contains("before_time")) {
        const auto& time = request->getParameter("before_time");
        const auto& before_id = request->getParameter("before_id");
        if (time.empty() || before_id.empty() || parameters.contains("page")) {
            throw RequestValidationError(
                "Send before_time and before_id together without page.");
        }
        comments = co_await services->social.older_comments(
            post, CommentCursor{time, positive_integer(before_id)},
            services->users);
    } else {
        comments = co_await services->social.thread(post, read_page(*request),
                                                    services->users);
    }
    if (!comments)
        co_return missing_post(*request);
    co_return api_ok(*request, std::move(*comments));
}

Task<HttpResponsePtr> create_comment(std::shared_ptr<Services> services,
                                     HttpRequestPtr request, std::string id) {
    const auto username = get_current_username(*request);
    const auto user = co_await services->users.find_id(username);
    if (!user)
        co_return unauthorized(*request);
    const auto post = positive_integer(id);
    auto comment = co_await services->social.comment(post, *user, username,
                                                     read_text(*request, 500));
    if (!comment)
        co_return missing_post(*request);
    co_return no_store(api_ok(*request, std::move(*comment), k201Created));
}

Task<HttpResponsePtr> set_like(std::shared_ptr<Services> services,
                               HttpRequestPtr request, std::string id,
                               bool liked) {
    const auto user =
        co_await services->users.find_id(get_current_username(*request));
    if (!user)
        co_return unauthorized(*request);
    auto like =
        co_await services->social.like(positive_integer(id), *user, liked);
    if (!like)
        co_return missing_post(*request);
    co_return no_store(api_ok(*request, *like));
}

}

void register_social_routes(drogon::HttpAppFramework& server,
                            const std::shared_ptr<Services>& services,
                            Auth& auth, const std::string& public_url_prefix) {
    server.registerHandler(
        "/",
        [services, public_url_prefix](HttpRequestPtr request) {
            return feed_page(services, std::move(request), public_url_prefix);
        },
        {Get});
    server.registerHandler("/social", &redirect_to_feed, {Get});
    server.registerHandler("/social/login",
                           [public_url_prefix](HttpRequestPtr) {
                               return login_page(public_url_prefix);
                           },
                           {Get});
    server.registerHandler("/api/search",
                           [services](HttpRequestPtr request) {
                               return search(services, std::move(request));
                           },
                           {Get, auth()});
    server.registerHandler("/api/posts/{1}",
                           [services](HttpRequestPtr request, std::string id) {
                               return view_post(services, std::move(request),
                                                std::move(id));
                           },
                           {Get, auth()});
    server.registerHandler(
        "/api/users/{1}/posts",
        [services](HttpRequestPtr request, std::string id) {
            return uncached_list(
                list_user_posts(services, std::move(request), std::move(id)));
        },
        {Get, auth()});
    server.registerHandler("/api/posts",
                           [services](HttpRequestPtr request) {
                               return uncached_list(
                                   list_posts(services, std::move(request)));
                           },
                           {Get, auth()});
    server.registerHandler("/api/posts",
                           [services](HttpRequestPtr request) {
                               return create_post(services, std::move(request));
                           },
                           {Post, auth()});
    server.registerHandler(
        "/api/posts/{1}/comments",
        [services](HttpRequestPtr request, std::string id) {
            return uncached_list(
                list_comments(services, std::move(request), std::move(id)));
        },
        {Get, auth()});
    server.registerHandler("/api/posts/{1}/comments",
                           [services](HttpRequestPtr request, std::string id) {
                               return create_comment(
                                   services, std::move(request), std::move(id));
                           },
                           {Post, auth()});
    server.registerHandler("/api/posts/{1}/like",
                           [services](HttpRequestPtr request, std::string id) {
                               return set_like(services, std::move(request),
                                               std::move(id), true);
                           },
                           {Put, auth()});
    server.registerHandler("/api/posts/{1}/like",
                           [services](HttpRequestPtr request, std::string id) {
                               return set_like(services, std::move(request),
                                               std::move(id), false);
                           },
                           {Delete, auth()});
}

}
