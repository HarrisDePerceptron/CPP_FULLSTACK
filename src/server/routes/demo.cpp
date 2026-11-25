#include "routes.hpp"
#include "server/json_response.hpp"

#include <drogon/drogon.h>

#include <map>

namespace APP_NAMESPACE {

void registerDemoRoutes() {
    using namespace drogon;
    app().registerHandler("/status", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        co_return apiOk(*request, std::map<std::string, bool>{{"ok", true}});
    }, {Get});
}

}
