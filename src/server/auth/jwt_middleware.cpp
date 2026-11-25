#include "app_namespace.hpp"
#include "jwt_middleware.hpp"
#include "tokens.hpp"
#include "server/json_response.hpp"

#include <drogon/HttpMiddleware.h>
#include <drogon/drogon.h>
#include <strings.h>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;

class JwtMiddleware : public HttpCoroMiddleware<JwtMiddleware, false> {
public:
    explicit JwtMiddleware(std::shared_ptr<const Tokens> tokens)
        : tokens_(std::move(tokens)) {}

    Task<HttpResponsePtr> invoke(const HttpRequestPtr& request,
                                 MiddlewareNextAwaiter&& next) override {
        const auto& header = request->getHeader("authorization");
        constexpr std::string_view prefix = "Bearer ";
        std::string_view token;
        if (header.empty()) {
            token = request->getCookie(sessionCookieName);
        } else if (header.size() > prefix.size() &&
                   strncasecmp(header.data(), prefix.data(), prefix.size()) == 0) {
            token = std::string_view(header).substr(prefix.size());
        }
        if (auto username = tokens_->verify(token)) {
            request->attributes()->insert(subjectAttribute, std::move(*username));
            co_return co_await std::move(next);
        }

        auto response = apiError(*request, k401Unauthorized, "UNAUTHORIZED", "A valid session or Bearer token is required.");
        response->addHeader("WWW-Authenticate", "Bearer");
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }

private:
    std::shared_ptr<const Tokens> tokens_;
};

}

std::string registerJwtMiddleware(std::shared_ptr<const Tokens> tokens) {
    drogon::app().registerMiddleware(std::make_shared<JwtMiddleware>(std::move(tokens)));
    return JwtMiddleware::classTypeName();
}

}
