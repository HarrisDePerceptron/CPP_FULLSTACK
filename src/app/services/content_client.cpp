#include "app_namespace.h"
#include "content_client.h"
#include "framework/support/validation.h"

#include <drogon/HttpClient.h>
#include <glaze/glaze.hpp>

#include <charconv>
#include <cstdlib>

namespace APP_NAMESPACE {

struct UpstreamContent {
    glz::raw_json_view id;
    std::string title;
    bool completed;
};

drogon::Task<std::expected<Content, ContentError>>
fetch_content(std::string origin, std::string path,
              std::chrono::milliseconds timeout) {
    auto client = drogon::HttpClient::newHttpClient(
        origin, trantor::EventLoop::getEventLoopOfCurrentThread());
#ifdef __APPLE__
    // Xmake's OpenSSL searches its own installation for a default CA bundle.
    if (client->secure() && !std::getenv("SSL_CERT_FILE") &&
        !std::getenv("SSL_CERT_DIR")) {
        client->addSSLConfigs({{"VerifyCAFile", "/etc/ssl/cert.pem"}});
    }
#endif
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(std::move(path));
    request->addHeader("Accept", "application/json");

    drogon::HttpResponsePtr response;
    try {
        response = co_await client->sendRequestCoro(
            std::move(request), std::chrono::duration<double>(timeout).count());
    } catch (const drogon::HttpException& error) {
        co_return std::unexpected(error.code() == drogon::ReqResult::Timeout
                                      ? ContentError::timeout
                                      : ContentError::upstream);
    }

    constexpr std::size_t body_limit = 256 * 1024;
    const auto body = response->body();
    if (response->statusCode() != drogon::k200OK || body.size() > body_limit) {
        co_return std::unexpected(ContentError::upstream);
    }

    constexpr glz::opts options{.error_on_unknown_keys = false,
                                .error_on_missing_keys = true};
    UpstreamContent content{};
    if (!utf8_code_point_count(body) || glz::validate_json(body) ||
        glz::read<options>(content, body)) {
        co_return std::unexpected(ContentError::upstream);
    }

    const auto id_text = trim_ascii_whitespace(content.id.str);
    if (id_text.empty())
        co_return std::unexpected(ContentError::upstream);
    std::int64_t id{};
    const auto end = id_text.data() + id_text.size();
    const auto [next, error] = std::from_chars(id_text.data(), end, id);
    if (error != std::errc{} || next != end)
        co_return std::unexpected(ContentError::upstream);
    co_return Content{id, std::move(content.title), content.completed};
}

}
