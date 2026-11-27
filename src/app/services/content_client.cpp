#include "app_namespace.h"
#include "content_client.h"

#include <drogon/HttpClient.h>
#include <simdjson.h>

#include <cstdlib>

namespace APP_NAMESPACE {

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

    simdjson::dom::parser parser;
    const simdjson::padded_string padded(body);
    simdjson::dom::element json;
    Content content{};
    std::string_view title;
    if (parser.parse(padded).get(json) || json["id"].get(content.id) ||
        json["title"].get(title) || json["completed"].get(content.completed)) {
        co_return std::unexpected(ContentError::upstream);
    }
    content.title = title;
    co_return content;
}

}
