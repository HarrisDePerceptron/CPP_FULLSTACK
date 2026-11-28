#pragma once

#include "app_namespace.h"

#include <memory>

namespace APP_NAMESPACE {

struct Services;
class ServerRuntime;

void register_app_routes(const ServerRuntime& runtime,
                         const std::shared_ptr<Services>& services);
void register_auth_routes(const ServerRuntime& runtime,
                          const std::shared_ptr<Services>& services);
void register_upload_routes(const ServerRuntime& runtime);
void register_content_routes(const ServerRuntime& runtime);
void register_demo_routes(const ServerRuntime& runtime);
void register_social_routes(const ServerRuntime& runtime,
                            const std::shared_ptr<Services>& services);

}
