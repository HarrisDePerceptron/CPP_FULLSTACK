#include "server_hooks.h"

#include "json_response.h"
#include "framework/support/workers.h"

#include <drogon/HttpAppFramework.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <exception>
#include <functional>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

void register_access_logging(drogon::HttpAppFramework& server, LogLevel level, Logger log,
                             std::string static_url_prefix) {
    using namespace drogon;
    using Clock = std::chrono::steady_clock;
    constexpr auto start_attribute = "app.requestStart";
    constexpr trantor::Logger::LogLevel levels[]{
        trantor::Logger::kInfo, trantor::Logger::kWarn, trantor::Logger::kError};
    server.setLogLevel(levels[static_cast<int>(level)]);
    server.registerPreRoutingAdvice([start_attribute](const HttpRequestPtr& request) {
        request->attributes()->insert(start_attribute, Clock::now());
        api_request_id(*request);
    });

    auto static_route = static_url_prefix + '*';
    server.registerPreSendingAdvice(
        [log, start_attribute, static_url_prefix = std::move(static_url_prefix),
         static_route = std::move(static_route)](const HttpRequestPtr& request, const HttpResponsePtr& response) {
            const auto start = request->attributes()->get<Clock::time_point>(start_attribute);
            if (start == Clock::time_point{})
                return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
            auto route = request->matchedPathPattern();
            if (route.empty()) {
                route = request->path().starts_with(static_url_prefix)
                            ? std::string_view(static_route)
                            : "(unmatched)";
            }
            log.write(LogLevel::info, "{} {} {} {} {}ms", api_request_id(*request), request->methodString(),
                      route, static_cast<int>(response->statusCode()), elapsed.count());
        });
}

void register_exception_handler(drogon::HttpAppFramework& server, Logger log) {
    using namespace drogon;
    server.setExceptionHandler([log](const std::exception& error, const HttpRequestPtr& request,
                                     std::function<void(const HttpResponsePtr&)>&& reply) {
        if (dynamic_cast<const WorkersBusy*>(&error)) {
            reply(api_error(*request, k503ServiceUnavailable, "SERVICE_UNAVAILABLE", "Server is busy. Try again shortly."));
            return;
        }
        log.write(LogLevel::error, "Request failed in {}", request->matchedPathPattern());
        reply(api_exception(*request, error));
    });
}

}
