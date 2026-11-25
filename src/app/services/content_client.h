#pragma once

#include "app/models/content_models.h"

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <expected>
#include <string>

namespace APP_NAMESPACE {

drogon::Task<std::expected<Content, ContentError>> fetch_content(
    std::string origin = "https://jsonplaceholder.typicode.com",
    std::string path = "/todos/1",
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

}
