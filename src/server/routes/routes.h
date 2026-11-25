#pragma once

#include "app_namespace.h"

#include <memory>
#include <string>

namespace APP_NAMESPACE {

struct Services;

void register_auth_routes(const std::shared_ptr<Services>& services, const std::string& jwt);
void register_upload_routes(const std::shared_ptr<Services>& services, const std::string& jwt);
void register_content_routes(const std::shared_ptr<Services>& services);
void register_demo_routes();
void register_social_routes(const std::shared_ptr<Services>& services, const std::string& jwt);

}
