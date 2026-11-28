#include "routes.h"

#include "framework/http/server_runtime.h"

namespace APP_NAMESPACE {

void register_app_routes(drogon::HttpAppFramework& server,
                         const std::shared_ptr<Services>& services,
                         const ServerRuntime& runtime) {
    register_demo_routes(server);
    register_auth_routes(server, services, runtime.tokens(), runtime.workers());
    register_social_routes(server, services, runtime.tokens(),
                           runtime.config().public_url_prefix);
    register_upload_routes(server, runtime.uploads(), runtime.workers(),
                           runtime.config().uploads_url_prefix);
    register_content_routes(server, runtime.log());
}

}
