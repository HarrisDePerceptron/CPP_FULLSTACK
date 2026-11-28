#pragma once

#include "app_namespace.h"

#include <memory>
#include <string>

namespace drogon {
class HttpAppFramework;
}

namespace APP_NAMESPACE {

class Auth;
struct Services;
struct ResolvedServerConfig;

void register_app_routes(drogon::HttpAppFramework& server,
                         const std::shared_ptr<Services>& services,
                         const ResolvedServerConfig& config);
void register_auth_routes(drogon::HttpAppFramework& server,
                          const std::shared_ptr<Services>& services,
                          Auth& auth);
void register_upload_routes(drogon::HttpAppFramework& server,
                            const std::shared_ptr<Services>& services,
                            Auth& auth, const std::string& uploads_url_prefix);
void register_content_routes(drogon::HttpAppFramework& server,
                             const std::shared_ptr<Services>& services);
void register_demo_routes(drogon::HttpAppFramework& server);
void register_social_routes(drogon::HttpAppFramework& server,
                            const std::shared_ptr<Services>& services,
                            Auth& auth, const std::string& public_url_prefix);

}
