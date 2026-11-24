#include "app_namespace.hpp"
#include "routes.hpp"
#include "server/json_response.hpp"
#include "server/services.hpp"
#include "services/content_client.hpp"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;

Task<HttpResponsePtr> content(std::shared_ptr<Services> services) {
    const auto result = co_await services->workers.submit([] { return fetchContent(); });
    if (result) co_return jsonResponse(*result);

    if (result.error() == ContentError::timeout) {
        services->log.write(LogLevel::warn, "Content provider timed out");
        co_return jsonError(k504GatewayTimeout, "The content provider timed out.");
    }
    services->log.write(LogLevel::warn, "Content provider returned an unusable response");
    co_return jsonError(k502BadGateway, "Could not fetch content from the provider.");
}

}

void registerContentRoutes(const std::shared_ptr<Services>& services) {
    auto& server = app();
    server.registerHandler("/api/content", [services](HttpRequestPtr) {
        return content(services);
    }, {Get});
}

}
