#include "app/server/routes.h"
#include "framework/http/json_response.h"

#include <drogon/drogon.h>

#include <map>

namespace APP_NAMESPACE {
namespace {

using namespace drogon;

Task<HttpResponsePtr> status(HttpRequestPtr request) {
    co_return api_ok(*request, std::map<std::string, bool>{{"ok", true}});
}

}

void register_demo_routes(drogon::HttpAppFramework& server) {
    server.registerHandler("/status", &status, {Get});
}

}
