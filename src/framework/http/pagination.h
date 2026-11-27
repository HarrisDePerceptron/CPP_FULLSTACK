#pragma once

#include "framework/support/pagination.h"
#include "request_validation.h"

#include <drogon/HttpRequest.h>

#include <charconv>
#include <limits>
#include <string_view>

namespace APP_NAMESPACE {

inline PageQuery read_page(const drogon::HttpRequest& request,
                           std::optional<std::int64_t> max_per_page = {}) {
    const auto parse = [](std::string_view text, std::string_view message) {
        if (!text.empty()) {
            std::int64_t value{};
            const auto end = text.data() + text.size();
            const auto [next, error] = std::from_chars(text.data(), end, value);
            if (error == std::errc{} && next == end)
                return value;
        }
        throw RequestValidationError(std::string(message));
    };

    const auto& params = request.parameters();
    std::int64_t page = 1;
    auto per_page = PageQuery::default_page_size;
    if (const auto it = params.find("page");
        it != params.end() && (max_per_page || !it->second.empty())) {
        page = parse(it->second,
                     max_per_page
                         ? "Expected an integer."
                         : "Use a positive integer for the post ID or page.");
    }
    if (max_per_page) {
        if (const auto it = params.find("per_page"); it != params.end())
            per_page = parse(it->second, "Expected an integer.");
        if (per_page > *max_per_page)
            throw RequestValidationError("Invalid pagination.");
    } else {
        if (page < 1)
            throw RequestValidationError(
                "Use a positive integer for the post ID or page.");
        if (page > std::numeric_limits<std::int64_t>::max() / per_page)
            throw RequestValidationError("Page is out of range.");
    }
    try {
        return PageQuery::numbered(page, per_page);
    } catch (const std::invalid_argument&) {
        throw RequestValidationError("Invalid pagination.");
    }
}

}
