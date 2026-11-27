#pragma once

#include "framework/db/database_backend.h"

#include <stdexcept>
#include "framework/http/request_validation.h"

namespace APP_NAMESPACE {

using CrudValidationError = RequestValidationError;

template <typename Dao, typename Resource> class CrudService {
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

    virtual drogon::Task<PagedResult<Response>> list(PageQuery page,
                                                     Filters filters) const {
        auto rows = co_await dao_.find()
                        .page(page)
                        .matching(std::move(filters))
                        .collect();
        co_return std::move(rows).map(
            [](const Record& row) { return Resource::to_response(row); });
    }

    virtual drogon::Task<std::optional<Response>> get(Id id) const {
        const auto row = co_await dao_.find_by_id(std::move(id)).collect();
        if (!row)
            co_return std::nullopt;
        co_return Resource::to_response(*row);
    }

    virtual drogon::Task<Response> create(Create input) const {
        std::optional<Response> result;
        co_await dao_.transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            const auto id = co_await dao_.create(
                tx, Resource::make_record(std::move(input)));
            const auto row = co_await dao_.find_by_id(tx, id).collect();
            if (!row)
                throw std::logic_error("Created row could not be read");
            result.emplace(Resource::to_response(*row));
        });
        co_return std::move(result).value();
    }

    virtual drogon::Task<std::optional<Response>> patch(Id id,
                                                        Patch input) const {
        std::optional<Response> result;
        co_await dao_.transaction([&](DaoTransaction& tx) -> drogon::Task<> {
            auto row = co_await dao_.find_by_id(tx, id).collect();
            if (!row)
                co_return;
            Resource::apply_patch(*row, std::move(input));
            if (!co_await dao_.update(tx, std::move(*row)))
                throw std::logic_error("Patch target disappeared");
            const auto updated = co_await dao_.find_by_id(tx, id).collect();
            if (!updated)
                throw std::logic_error("Updated row could not be read");
            result.emplace(Resource::to_response(*updated));
        });
        co_return result;
    }

    virtual drogon::Task<bool> remove(Id id) const {
        return dao_.delete_by_id(std::move(id));
    }

  protected:
    const Dao& dao() const { return dao_; }

  private:
    Dao dao_;
};

}
