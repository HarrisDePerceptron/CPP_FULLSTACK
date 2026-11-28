#include "routes.h"

namespace APP_NAMESPACE {

void register_app_routes(const ServerRuntime& runtime,
                         const std::shared_ptr<Services>& services) {
    register_demo_routes(runtime);
    register_auth_routes(runtime, services);
    register_social_routes(runtime, services);
    register_upload_routes(runtime);
    register_content_routes(runtime);
}

}
