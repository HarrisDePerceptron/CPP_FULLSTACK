#pragma once

#include "db/database_backend.hpp"

#include <stdexcept>

namespace APP_NAMESPACE {

class CrudValidationError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CrudPage {
    std::int64_t limit = 100;
    std::int64_t offset = 0;
};

template <typename Dao, typename Resource>
class CrudService {
public:
    using Policy = Resource;
    using Record = typename Dao::RecordType;
    using Id = typename Record::Id;
    using Response = typename Resource::Response;
    using Create = typename Resource::Create;
    using Patch = typename Resource::Patch;
    using Filters = typename Dao::EqualityFilters;

    explicit CrudService(Dao dao) : dao_(std::move(dao)) {}
    virtual ~CrudService() = default;

    virtual drogon::Task<std::vector<Response>> list(CrudPage page, Filters filters) const {
        const auto rows = co_await dao_.findMatching(page.limit, page.offset, std::move(filters));
        std::vector<Response> result;
        result.reserve(rows.size());
        for (const auto& row : rows) result.push_back(Resource::toResponse(row));
        co_return result;
    }

    virtual drogon::Task<std::optional<Response>> get(Id id) const {
        const auto row = co_await dao_.findById(std::move(id));
        if (!row) co_return std::nullopt;
        co_return Resource::toResponse(*row);
    }

    virtual drogon::Task<Response> create(Create input) const {
        std::optional<Response> result;
        co_await dao_.transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            const auto id = co_await dao_.create(tx, Resource::makeRecord(std::move(input)));
            const auto row = co_await dao_.findById(tx, id);
            if (!row) throw std::logic_error("Created row could not be read");
            result.emplace(Resource::toResponse(*row));
        });
        co_return std::move(result).value();
    }

    virtual drogon::Task<std::optional<Response>> patch(Id id, Patch input) const {
        std::optional<Response> result;
        co_await dao_.transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            auto row = co_await dao_.findById(tx, id);
            if (!row) co_return;
            Resource::applyPatch(*row, std::move(input));
            if (!co_await dao_.update(tx, std::move(*row))) throw std::logic_error("Patch target disappeared");
            const auto updated = co_await dao_.findById(tx, id);
            if (!updated) throw std::logic_error("Updated row could not be read");
            result.emplace(Resource::toResponse(*updated));
        });
        co_return result;
    }

    virtual drogon::Task<bool> remove(Id id) const {
        return dao_.deleteById(std::move(id));
    }

protected:
    const Dao& dao() const { return dao_; }

private:
    Dao dao_;
};

}
