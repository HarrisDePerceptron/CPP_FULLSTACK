#include "routes.h"

#include "framework/auth/jwt_middleware.h"
#include "framework/http/server.h"
#include "services.h"

namespace APP_NAMESPACE {

void register_app_routes(drogon::HttpAppFramework& server, const std::shared_ptr<Services>& services,
                         const ResolvedServerConfig& config) {
    const auto jwt = register_jwt_middleware(server, {services, &services->tokens});
    register_demo_routes(server);
    register_auth_routes(server, services, jwt);
    register_social_routes(server, services, jwt, config.public_url_prefix);
    register_upload_routes(server, services, jwt, config.uploads_url_prefix);
    register_content_routes(server, services);
}

}
