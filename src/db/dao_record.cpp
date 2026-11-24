#include "dao_record.hpp"

#include <charconv>
#include <format>
#include <stdexcept>

namespace APP_NAMESPACE::detail {

DaoDateTime parseDaoDateTime(std::string_view text) {
    using namespace std::chrono;
    constexpr auto invalid = "invalid DAO UTC datetime";
    if (text.size() != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
        throw std::runtime_error(invalid);
    }

    const auto number = [&](std::size_t offset, std::size_t size) {
        unsigned value{};
        const auto* end = text.data() + offset + size;
        const auto result = std::from_chars(text.data() + offset, end, value);
        if (result.ec != std::errc{} || result.ptr != end) throw std::runtime_error(invalid);
        return value;
    };
    const auto date = year{static_cast<int>(number(0, 4))} / month{number(5, 2)} / day{number(8, 2)};
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!date.ok() || hour >= 24 || minute >= 60 || second >= 60) throw std::runtime_error(invalid);
    return sys_days{date} + hours{hour} + minutes{minute} + seconds{second} + milliseconds{number(20, 3)};
}

std::string formatDaoDateTime(DaoDateTime value) {
    return std::format("{:%FT%T}Z", value);
}

}
