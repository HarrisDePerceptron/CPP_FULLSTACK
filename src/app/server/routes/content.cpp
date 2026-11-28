#include "app_namespace.h"
#include "app/server/routes.h"
#include "framework/http/json_response.h"
#include "framework/support/log.h"
#include "app/services/content_client.h"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;

Task<HttpResponsePtr> content(const Logger& log, HttpRequestPtr request) {
    const auto result = co_await fetch_content();
    if (result)
        co_return api_ok(*request, *result);

    if (result.error() == ContentError::timeout) {
        log.write(LogLevel::warn, "Content provider timed out");
        co_return api_error(*request, k504GatewayTimeout, "GATEWAY_TIMEOUT",
                            "The content provider timed out.");
    }
    log.write(LogLevel::warn, "Content provider returned an unusable response");
    co_return api_error(*request, k502BadGateway, "BAD_GATEWAY",
                        "Could not fetch content from the provider.");
}

}

void register_content_routes(drogon::HttpAppFramework& server,
                             const Logger& log) {
    server.registerHandler("/api/content",
                           [&log](HttpRequestPtr request) {
                               return content(log, std::move(request));
                           },
                           {Get});
}

}
