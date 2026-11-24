#include "app_namespace.hpp"
#include "routes.hpp"

#include <drogon/drogon.h>

#include <ctime>
#include <format>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;

std::string serverTime() {
    const auto now = std::time(nullptr);
    std::tm local{};
    if (now == std::time_t{-1} || !localtime_r(&now, &local)) {
        throw std::runtime_error("could not read server time");
    }
    const int hour = (local.tm_hour + 11) % 12 + 1;
    return std::format("{:02}/{:02}/{:04} {:02}:{:02} {}", local.tm_mday,
                       local.tm_mon + 1, local.tm_year + 1900, hour, local.tm_min,
                       local.tm_hour < 12 ? "am" : "pm");
}

}

void registerDemoRoutes() {
    auto& server = app();
    server.registerHandler("/", [](HttpRequestPtr) -> Task<HttpResponsePtr> {
        HttpViewData data;
        data.insert("server_time", serverTime());
        auto response = HttpResponse::newHttpViewResponse("AppViews::index", data);
        response->addHeader("Cache-Control", "no-store");
        co_return response;
    }, {Get});
    server.registerHandler("/status", [](HttpRequestPtr) -> Task<HttpResponsePtr> {
        auto response = HttpResponse::newHttpResponse();
        response->setContentTypeCode(CT_TEXT_PLAIN);
        response->setBody("OK");
        co_return response;
    }, {Get});
}

}
