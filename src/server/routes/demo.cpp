#include "routes.h"
#include "server/json_response.h"

#include <drogon/drogon.h>

#include <map>

namespace APP_NAMESPACE {

void register_demo_routes() {
    using namespace drogon;
    app().registerHandler("/status", [](HttpRequestPtr request) -> Task<HttpResponsePtr> {
        co_return api_ok(*request, std::map<std::string, bool>{{"ok", true}});
    }, {Get});
}

}
