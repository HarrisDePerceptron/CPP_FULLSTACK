#include "dao_datetime.h"

#include <format>

namespace APP_NAMESPACE::detail {

std::string format_dao_date_time(DaoDateTime value) {
    using namespace std::chrono;
    if (value < sys_days{year{0} / January / 1} ||
        value >= sys_days{year{10000} / January / 1}) {
        throw std::out_of_range("DAO UTC datetime must have a four-digit year");
    }
    return std::format("{:%FT%T}Z", value);
}

}
