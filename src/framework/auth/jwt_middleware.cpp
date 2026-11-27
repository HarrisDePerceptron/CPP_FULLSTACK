#include "app_namespace.h"
#include "jwt_middleware.h"
#include "tokens.h"
#include "framework/http/json_response.h"

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
            token = request->getCookie(session_cookie_name);
        } else if (header.size() > prefix.size() &&
                   strncasecmp(header.data(), prefix.data(), prefix.size()) ==
                       0) {
            token = std::string_view(header).substr(prefix.size());
        }
        if (auto username = tokens_->verify(token)) {
            request->attributes()->insert(subject_attribute,
                                          std::move(*username));
            co_return co_await std::move(next);
        }

        auto response =
            api_error(*request, k401Unauthorized, "UNAUTHORIZED",
                      "A valid session or Bearer token is required.");
        response->addHeader("WWW-Authenticate", "Bearer");
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }

  private:
    std::shared_ptr<const Tokens> tokens_;
};

}

std::string register_jwt_middleware(drogon::HttpAppFramework& server,
                                    std::shared_ptr<const Tokens> tokens) {
    server.registerMiddleware(
        std::make_shared<JwtMiddleware>(std::move(tokens)));
    return JwtMiddleware::classTypeName();
}

}
