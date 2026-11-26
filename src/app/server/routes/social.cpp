#include "app/models/social_models.h"
#include "app/server/routes.h"
#include "framework/auth/jwt_middleware.h"
#include "framework/http/json_response.h"
#include "app/server/services.h"

#include <drogon/drogon.h>

#include <charconv>
#include <limits>

namespace APP_NAMESPACE {

namespace {

using namespace drogon;

std::int64_t positive_integer(std::string_view value) {
    std::int64_t number{};
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < 1) {
        throw RequestValidationError("Use a positive integer for the post ID or page.");
    }
    return number;
}

std::int64_t page_number(const HttpRequest& request) {
    const auto& value = request.getParameter("page");
    const auto page = value.empty() ? 1 : positive_integer(value);
    if (page > std::numeric_limits<std::int64_t>::max() / SocialService::page_size) {
        throw RequestValidationError("Page is out of range.");
    }
    return page;
}

std::string read_text(const HttpRequest& request) {
    SocialTextBody payload;
    const auto body = request.body();
    if (request.contentType() != CT_APPLICATION_JSON || body.size() > 8192 ||
        glz::validate_json(body) || glz::read<glz::opts{.error_on_missing_keys = true}>(payload, body)) {
        throw RequestValidationError("Send a JSON text field.");
    }
    return std::move(payload.text);
}

template <typename Item>
HttpResponsePtr page_response(const HttpRequest& request, std::vector<Item> items, std::int64_t page,
                             std::int64_t per_page = SocialService::page_size) {
    const bool has_more = items.size() > static_cast<std::size_t>(per_page);
    if (has_more) items.pop_back();
    return api_ok(request, std::move(items), ApiPagination{page, per_page, has_more});
}

HttpResponsePtr missing_post(const HttpRequest& request) {
    return api_error(request, k404NotFound, "NOT_FOUND", "That post could not be found.");
}

HttpResponsePtr unauthorized(const HttpRequest& request) {
    return api_error(request, k401Unauthorized, "UNAUTHORIZED", "Sign in to use the social feed.");
}

HttpResponsePtr no_store(HttpResponsePtr response) {
    response->addHeader("Cache-Control", "no-store");
    return response;
}

HttpResponsePtr render_page(const std::string& view, const std::string& public_url_prefix) {
    HttpViewData data;
    data.insert("public_url_prefix", public_url_prefix);
    return no_store(HttpResponse::newHttpViewResponse(view, data));
}

Task<HttpResponsePtr> feed_page(std::shared_ptr<Services> services, HttpRequestPtr request,
                                const std::string& public_url_prefix) {
    const auto username = services->tokens.verify(request->getCookie(session_cookie_name));
    if (!username || !co_await services->users.find_id(*username)) {
        co_return no_store(HttpResponse::newRedirectionResponse("/social/login"));
    }
    co_return render_page("AppViews::social", public_url_prefix);
}

Task<HttpResponsePtr> redirect_to_feed(HttpRequestPtr) {
    co_return no_store(HttpResponse::newRedirectionResponse("/"));
}

Task<HttpResponsePtr> login_page(const std::string& public_url_prefix) {
    co_return render_page("AppViews::social_login", public_url_prefix);
}

Task<HttpResponsePtr> search(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto viewer = co_await services->users.find_id(request->attributes()->get<std::string>(subject_attribute));
    if (!viewer) co_return unauthorized(*request);
    auto matches = co_await services->social.search(request->getParameter("q"), services->users);
    co_return no_store(api_ok(*request, std::move(matches)));
}

Task<HttpResponsePtr> view_post(std::shared_ptr<Services> services, HttpRequestPtr request, std::string id) {
    const auto viewer = co_await services->users.find_id(request->attributes()->get<std::string>(subject_attribute));
    if (!viewer) co_return unauthorized(*request);
    auto post = co_await services->social.view(*viewer, positive_integer(id));
    if (!post) co_return missing_post(*request);
    co_return no_store(api_ok(*request, std::move(*post)));
}

Task<HttpResponsePtr> list_user_posts(std::shared_ptr<Services> services, HttpRequestPtr request,
                                      std::string id) {
    const auto viewer = co_await services->users.find_id(request->attributes()->get<std::string>(subject_attribute));
    if (!viewer) co_return unauthorized(*request);
    const auto page = page_number(*request);
    auto profile = co_await services->social.user_posts(*viewer, positive_integer(id), page, services->users);
    if (!profile) co_return api_error(*request, k404NotFound, "NOT_FOUND", "That person could not be found.");
    co_return no_store(page_response(*request, std::move(*profile), page));
}

Task<HttpResponsePtr> list_posts(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto user = co_await services->users.find_id(request->attributes()->get<std::string>(subject_attribute));
    if (!user) co_return unauthorized(*request);
    const auto page = page_number(*request);
    auto posts = co_await services->social.feed(*user, page);
    co_return no_store(page_response(*request, std::move(posts), page));
}

Task<HttpResponsePtr> create_post(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto username = request->attributes()->get<std::string>(subject_attribute);
    const auto user = co_await services->users.find_id(username);
    if (!user) co_return unauthorized(*request);
    auto post = co_await services->social.post(*user, username, read_text(*request));
    co_return no_store(api_ok(*request, std::move(post), k201Created));
}

Task<HttpResponsePtr> list_comments(std::shared_ptr<Services> services, HttpRequestPtr request,
                                    std::string id) {
    const auto user = co_await services->users.find_id(request->attributes()->get<std::string>(subject_attribute));
    if (!user) co_return unauthorized(*request);
    const auto post = positive_integer(id);
    const auto& parameters = request->getParameters();
    if (parameters.contains("before_id") || parameters.contains("before_time")) {
        const auto& time = request->getParameter("before_time");
        const auto& before_id = request->getParameter("before_id");
        if (time.empty() || before_id.empty() || parameters.contains("page")) {
            throw RequestValidationError("Send before_time and before_id together without page.");
        }
        auto comments = co_await services->social.older_comments(
            post, CommentCursor{time, positive_integer(before_id)}, services->users);
        if (!comments) co_return missing_post(*request);
        co_return no_store(page_response(*request, std::move(*comments), 1, SocialService::comment_batch_size));
    }
    const auto page = page_number(*request);
    auto comments = co_await services->social.thread(post, page, services->users);
    if (!comments) co_return missing_post(*request);
    co_return no_store(page_response(*request, std::move(*comments), page));
}

Task<HttpResponsePtr> create_comment(std::shared_ptr<Services> services, HttpRequestPtr request,
                                     std::string id) {
    const auto username = request->attributes()->get<std::string>(subject_attribute);
    const auto user = co_await services->users.find_id(username);
    if (!user) co_return unauthorized(*request);
    const auto post = positive_integer(id);
    auto comment = co_await services->social.comment(post, *user, username, read_text(*request));
    if (!comment) co_return missing_post(*request);
    co_return no_store(api_ok(*request, std::move(*comment), k201Created));
}

Task<HttpResponsePtr> set_like(std::shared_ptr<Services> services, HttpRequestPtr request,
                               std::string id, bool liked) {
    const auto user = co_await services->users.find_id(request->attributes()->get<std::string>(subject_attribute));
    if (!user) co_return unauthorized(*request);
    auto like = co_await services->social.like(positive_integer(id), *user, liked);
    if (!like) co_return missing_post(*request);
    co_return no_store(api_ok(*request, *like));
}

}

void register_social_routes(drogon::HttpAppFramework& server, const std::shared_ptr<Services>& services,
                            const std::string& jwt, const std::string& public_url_prefix) {
    server.registerHandler("/", [services, public_url_prefix](HttpRequestPtr request) {
        return feed_page(services, std::move(request), public_url_prefix);
    }, {Get});
    server.registerHandler("/social", &redirect_to_feed, {Get});
    server.registerHandler("/social/login", [public_url_prefix](HttpRequestPtr) {
        return login_page(public_url_prefix);
    }, {Get});
    server.registerHandler("/api/search", [services](HttpRequestPtr request) {
        return search(services, std::move(request));
    }, {Get, jwt});
    server.registerHandler("/api/posts/{1}", [services](HttpRequestPtr request, std::string id) {
        return view_post(services, std::move(request), std::move(id));
    }, {Get, jwt});
    server.registerHandler("/api/users/{1}/posts", [services](HttpRequestPtr request, std::string id) {
        return list_user_posts(services, std::move(request), std::move(id));
    }, {Get, jwt});
    server.registerHandler("/api/posts", [services](HttpRequestPtr request) {
        return list_posts(services, std::move(request));
    }, {Get, jwt});
    server.registerHandler("/api/posts", [services](HttpRequestPtr request) {
        return create_post(services, std::move(request));
    }, {Post, jwt});
    server.registerHandler("/api/posts/{1}/comments", [services](HttpRequestPtr request, std::string id) {
        return list_comments(services, std::move(request), std::move(id));
    }, {Get, jwt});
    server.registerHandler("/api/posts/{1}/comments", [services](HttpRequestPtr request, std::string id) {
        return create_comment(services, std::move(request), std::move(id));
    }, {Post, jwt});
    server.registerHandler("/api/posts/{1}/like", [services](HttpRequestPtr request, std::string id) {
        return set_like(services, std::move(request), std::move(id), true);
    }, {Put, jwt});
    server.registerHandler("/api/posts/{1}/like", [services](HttpRequestPtr request, std::string id) {
        return set_like(services, std::move(request), std::move(id), false);
    }, {Delete, jwt});
}

}
