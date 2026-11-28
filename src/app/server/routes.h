#pragma once

#include "app_namespace.h"

#include <memory>

namespace APP_NAMESPACE {

struct Services;
class ServerRuntime;

void register_app_routes(ServerRuntime& runtime,
                         const std::shared_ptr<Services>& services);
void register_auth_routes(ServerRuntime& runtime,
                          const std::shared_ptr<Services>& services);
void register_upload_routes(ServerRuntime& runtime);
void register_content_routes(ServerRuntime& runtime);
void register_demo_routes(ServerRuntime& runtime);
void register_social_routes(ServerRuntime& runtime,
                            const std::shared_ptr<Services>& services);

}
