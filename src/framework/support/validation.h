#pragma once

#include "app_namespace.h"

#include <concepts>
#include <stdexcept>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

template <std::integral Number>
void require_positive_number(Number value, Number maximum,
                             std::string_view name) {
    if (value < 1 || value > maximum) {
        throw std::runtime_error(std::string(name) + " must be between 1 and " +
                                 std::to_string(maximum));
    }
}

}
