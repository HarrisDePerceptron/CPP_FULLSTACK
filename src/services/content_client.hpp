#pragma once

#include "app_namespace.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

struct Content {
    std::int64_t id;
    std::string title;
    bool completed;
};

enum class ContentError { timeout, upstream };

std::expected<Content, ContentError> fetchContent(
    std::string_view url = "https://jsonplaceholder.typicode.com/todos/1",
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

}
