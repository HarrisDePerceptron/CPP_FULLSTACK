#pragma once

#include "app_namespace.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace APP_NAMESPACE {

using DaoDateTime = std::chrono::sys_time<std::chrono::milliseconds>;

namespace detail {

constexpr DaoDateTime parse_dao_date_time(std::string_view text) {
    using namespace std::chrono;
    constexpr auto invalid = "invalid DAO UTC datetime";
    if (text.size() != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
        throw std::runtime_error(invalid);
    }

    const auto number = [&](std::size_t offset, std::size_t size) {
        unsigned value{};
        for (std::size_t i = 0; i < size; ++i) {
            const auto digit = text[offset + i];
            if (digit < '0' || digit > '9') throw std::runtime_error(invalid);
            value = value * 10 + static_cast<unsigned>(digit - '0');
        }
        return value;
    };
    const auto date = year{static_cast<int>(number(0, 4))} / month{number(5, 2)} / day{number(8, 2)};
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!date.ok() || hour >= 24 || minute >= 60 || second >= 60) throw std::runtime_error(invalid);
    return sys_days{date} + hours{hour} + minutes{minute} + seconds{second} + milliseconds{number(20, 3)};
}

std::string format_dao_date_time(DaoDateTime value);

}

}
