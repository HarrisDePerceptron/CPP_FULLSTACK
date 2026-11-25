#pragma once

#include "app_namespace.h"

#include <cstdint>
#include <string>

namespace APP_NAMESPACE {

struct Content {
    std::int64_t id;
    std::string title;
    bool completed;
};

enum class ContentError { timeout, upstream };

}
