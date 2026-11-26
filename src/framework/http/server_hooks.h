#pragma once

#include "app_namespace.h"
#include "framework/support/log.h"

#include <string>

namespace drogon {
class HttpAppFramework;
}

namespace APP_NAMESPACE {

void register_access_logging(drogon::HttpAppFramework& server, LogLevel level, Logger log,
                             std::string static_url_prefix);
void register_exception_handler(drogon::HttpAppFramework& server, Logger log);

}
