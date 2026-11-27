#pragma once

#include "app_namespace.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

inline std::string_view trim_ascii_whitespace(std::string_view text) {
    constexpr auto whitespace = " \r\n\t\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

inline std::optional<std::size_t> utf8_code_point_count(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++count) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        if (lead <= 0x7f)
            continue;

        unsigned code_point;
        unsigned minimum;
        std::size_t trailing;
        if (lead >= 0xc2 && lead <= 0xdf) {
            code_point = lead & 0x1f;
            minimum = 0x80;
            trailing = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            code_point = lead & 0x0f;
            minimum = 0x800;
            trailing = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            code_point = lead & 0x07;
            minimum = 0x10000;
            trailing = 3;
        } else {
            return std::nullopt;
        }
        if (trailing > text.size() - index)
            return std::nullopt;
        for (std::size_t offset = 0; offset < trailing; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index++]);
            if ((byte & 0xc0) != 0x80)
                return std::nullopt;
            code_point = (code_point << 6) | (byte & 0x3f);
        }
        if (code_point < minimum || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff))
            return std::nullopt;
    }
    return count;
}

template <std::integral Number>
void require_positive_number(Number value, Number maximum,
                             std::string_view name) {
    if (value < 1 || value > maximum) {
        throw std::runtime_error(std::string(name) + " must be between 1 and " +
                                 std::to_string(maximum));
    }
}

}
