#pragma once

#include "app_namespace.h"

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

namespace APP_NAMESPACE {

struct Content {
    std::int64_t id;
    std::string title;
    bool completed;
};

enum class ContentError { timeout, upstream };

drogon::Task<std::expected<Content, ContentError>>
fetch_content(std::string origin = "https://jsonplaceholder.typicode.com",
              std::string path = "/todos/1",
              std::chrono::milliseconds timeout = std::chrono::seconds(5));

}
