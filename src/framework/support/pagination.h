#pragma once

#include "app_namespace.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace APP_NAMESPACE {

template <typename T> struct PagedResult;

class PageQuery {
  public:
    static constexpr std::int64_t default_page_size = 20;

    static PageQuery window(std::int64_t limit, std::int64_t offset) {
        if (limit < 1 || offset < 0)
            throw std::invalid_argument("invalid pagination");
        return PageQuery(limit, offset);
    }

    static PageQuery numbered(std::int64_t page,
                              std::int64_t per_page = default_page_size) {
        if (page < 1 || per_page < 1 ||
            page - 1 > std::numeric_limits<std::int64_t>::max() / per_page) {
            throw std::invalid_argument("invalid pagination");
        }
        return PageQuery(per_page, (page - 1) * per_page);
    }

    std::int64_t limit() const { return limit_; }
    std::int64_t offset() const { return offset_; }

    std::int64_t fetch_limit() const {
        return limit_ == std::numeric_limits<std::int64_t>::max() ? limit_
                                                                  : limit_ + 1;
    }

    std::optional<std::int64_t> page_number() const {
        const auto index = offset_ / limit_;
        if (offset_ % limit_ != 0 ||
            index == std::numeric_limits<std::int64_t>::max())
            return std::nullopt;
        return index + 1;
    }

    template <typename T> PagedResult<T> finish(std::vector<T> items) const;

  private:
    PageQuery(std::int64_t limit, std::int64_t offset)
        : limit_(limit), offset_(offset) {}

    std::int64_t limit_;
    std::int64_t offset_;
};

template <typename T> struct PagedResult {
    std::vector<T> items;
    PageQuery query;
    bool has_more;

    template <typename Mapper> auto map(Mapper mapper) && {
        using Item = std::remove_cvref_t<std::invoke_result_t<Mapper&, T&&>>;
        std::vector<Item> mapped;
        mapped.reserve(items.size());
        for (auto&& item : items)
            mapped.push_back(std::invoke(mapper, std::move(item)));
        return PagedResult<Item>{std::move(mapped), query, has_more};
    }
};

template <typename T>
PagedResult<T> PageQuery::finish(std::vector<T> items) const {
    const auto has_more = std::cmp_greater(items.size(), limit_);
    if (has_more)
        items.erase(items.begin() + limit_, items.end());
    return {std::move(items), *this, has_more};
}

}
