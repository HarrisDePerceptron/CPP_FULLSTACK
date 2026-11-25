#pragma once

#include "app_namespace.hpp"

#include <memory>
#include <string>

namespace APP_NAMESPACE {

struct Services;

void registerAuthRoutes(const std::shared_ptr<Services>& services, const std::string& jwt);
void registerUploadRoutes(const std::shared_ptr<Services>& services, const std::string& jwt);
void registerContentRoutes(const std::shared_ptr<Services>& services);
void registerDemoRoutes();
void registerSocialRoutes(const std::shared_ptr<Services>& services, const std::string& jwt);

}
