#pragma once

#include "app_namespace.h"

#include <memory>
#include <string>

namespace drogon {
class HttpAppFramework;
}

namespace APP_NAMESPACE {

struct Services;
class ServerRuntime;
class Tokens;
class Workers;
class Uploads;
class Logger;

void register_app_routes(drogon::HttpAppFramework& server,
                         const std::shared_ptr<Services>& services,
                         const ServerRuntime& runtime);
void register_auth_routes(drogon::HttpAppFramework& server,
                          const std::shared_ptr<Services>& services,
                          const Tokens& tokens, Workers& workers);
void register_upload_routes(drogon::HttpAppFramework& server,
                            const Uploads& uploads, Workers& workers,
                            const std::string& uploads_url_prefix);
void register_content_routes(drogon::HttpAppFramework& server,
                             const Logger& log);
void register_demo_routes(drogon::HttpAppFramework& server);
void register_social_routes(drogon::HttpAppFramework& server,
                            const std::shared_ptr<Services>& services,
                            const Tokens& tokens,
                            const std::string& public_url_prefix);

}
