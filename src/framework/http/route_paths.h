#pragma once

#include "app_namespace.h"

#include <drogon/HttpTypes.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace APP_NAMESPACE {

template <std::size_t Size> struct RoutePath {
    char value[Size]{};

    constexpr RoutePath(const char (&path)[Size]) {
        for (std::size_t index = 0; index < Size; ++index)
            value[index] = path[index];
    }

    constexpr std::string_view view() const { return {value, Size - 1}; }
};

namespace detail {

constexpr bool is_parameter(std::string_view segment) {
    return segment.starts_with('{') && segment.ends_with('}');
}

constexpr bool is_literal_char(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_' ||
           value == '~';
}

constexpr std::string_view next_route_segment(std::string_view& path) {
    const auto slash = path.find('/');
    const auto segment = path.substr(0, slash);
    path.remove_prefix(slash == path.npos ? path.size() : slash + 1);
    return segment;
}

constexpr bool valid_route_path(std::string_view path,
                                bool allow_parameters = true) {
    if (!path.starts_with('/') || path.find("//") != path.npos)
        return false;
    path.remove_prefix(1);
    while (!path.empty()) {
        auto segment = next_route_segment(path);
        if (allow_parameters && is_parameter(segment)) {
            segment.remove_prefix(1);
            segment.remove_suffix(1);
            if (segment.empty() || segment.front() == '0')
                return false;
            for (char value : segment) {
                if (value < '0' || value > '9')
                    return false;
            }
        } else {
            for (char value : segment) {
                if (!is_literal_char(value))
                    return false;
            }
        }
    }
    return true;
}

constexpr bool valid_route_prefix(std::string_view prefix) {
    return prefix.empty() || valid_route_path(prefix, false);
}

struct RoutePathParts {
    std::string_view prefix;
    std::string_view separator;
    std::string_view suffix;
};

constexpr RoutePathParts join_route_parts(std::string_view prefix,
                                          std::string_view suffix) {
    if (prefix.ends_with('/'))
        prefix.remove_suffix(1);
    const bool trailing_slash = suffix == "/";
    if (suffix.starts_with('/'))
        suffix.remove_prefix(1);
    const auto separator =
        !suffix.empty() || prefix.empty() || trailing_slash ? "/" : "";
    return {prefix, separator, suffix};
}

constexpr char fold_route_char(char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

constexpr bool equal_route_segment(std::string_view left,
                                   std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold_route_char(left[index]) != fold_route_char(right[index]))
            return false;
    }
    return true;
}

struct RouteMatch {
    drogon::HttpMethod method;
    std::string_view path;
};

inline constexpr auto all_route_methods = drogon::Invalid;

constexpr bool routes_clash(RouteMatch left, RouteMatch right) {
    if (left.method != right.method && left.method != all_route_methods &&
        right.method != all_route_methods)
        return false;
    // Literal paths take precedence over the parameterized router.
    if ((left.path.find('{') == left.path.npos) !=
        (right.path.find('{') == right.path.npos))
        return false;

    for (;;) {
        const bool left_last = left.path.find('/') == left.path.npos;
        const bool right_last = right.path.find('/') == right.path.npos;
        if (left_last != right_last)
            return false;
        const auto a = next_route_segment(left.path);
        const auto b = next_route_segment(right.path);
        if (!is_parameter(a) && !is_parameter(b) && !equal_route_segment(a, b))
            return false;
        if (left_last)
            return true;
    }
}

template <drogon::HttpMethod Method, RoutePath Prefix, RoutePath Suffix>
struct RouteKey {
    static constexpr auto storage = [] {
        constexpr auto parts = join_route_parts(Prefix.view(), Suffix.view());
        std::array<char, parts.prefix.size() + parts.separator.size() +
                             parts.suffix.size()>
            path{};
        auto output = path.begin();
        for (const auto part : {parts.prefix, parts.separator, parts.suffix}) {
            for (const char value : part)
                *output++ = value;
        }
        return path;
    }();

    static constexpr RouteMatch match{Method, {storage.data(), storage.size()}};
};

template <typename Candidate, typename... Existing>
concept DistinctRoute =
    (!routes_clash(Candidate::match, Existing::match) && ...);

template <typename... Keys> struct RouteKeys {
    template <typename... Other>
    static constexpr bool can_merge = (DistinctRoute<Other, Keys...> && ...);
};

}
}
