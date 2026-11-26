#include "app_namespace.h"
#include "app/server/routes.h"
#include "framework/http/json_response.h"
#include "app/server/services.h"
#include "app/services/content_client.h"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;

Task<HttpResponsePtr> content(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto result = co_await fetch_content();
    if (result) co_return api_ok(*request, *result);

    if (result.error() == ContentError::timeout) {
        services->log.write(LogLevel::warn, "Content provider timed out");
        co_return api_error(*request, k504GatewayTimeout, "GATEWAY_TIMEOUT", "The content provider timed out.");
    }
    services->log.write(LogLevel::warn, "Content provider returned an unusable response");
    co_return api_error(*request, k502BadGateway, "BAD_GATEWAY", "Could not fetch content from the provider.");
}

}

void register_content_routes(drogon::HttpAppFramework& server, const std::shared_ptr<Services>& services) {
    server.registerHandler("/api/content", [services](HttpRequestPtr request) {
        return content(services, std::move(request));
    }, {Get});
}

}
