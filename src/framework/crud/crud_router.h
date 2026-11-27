#pragma once

#include "framework/http/json_response.h"
#include "crud_service.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/Exception.h>

#include <charconv>
#include <concepts>
#include <limits>

namespace APP_NAMESPACE {

struct CrudMethods {
    bool list = true;
    bool get = true;
    bool create = true;
    bool patch = true;
    bool remove = true;
};

template <std::integral Integer>
Integer parse_crud_integer(std::string_view text) {
    if (text.empty())
        throw CrudValidationError("Expected an integer.");
    Integer value{};
    const auto end = text.data() + text.size();
    const auto [next, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || next != end)
        throw CrudValidationError("Expected an integer.");
    return value;
}

template <typename Service> class CrudRouter {
  public:
    using Response = typename Service::Response;
    using Id = typename Service::Id;
    using Policy = typename Service::Policy;

    explicit CrudRouter(std::shared_ptr<const Service> service,
                        CrudMethods methods = {})
        : service_(std::move(service)), methods_(methods) {
        if (!service_)
            throw std::invalid_argument("CRUD router requires a service");
    }
    virtual ~CrudRouter() = default;

    static void mount(drogon::HttpAppFramework& server, std::string path,
                      std::shared_ptr<const CrudRouter> router,
                      const std::vector<std::string>& middleware = {}) {
        if (!router)
            throw std::invalid_argument("CRUD mount requires a router");
        std::vector<drogon::internal::HttpConstraint> constraints;
        for (const auto& name : middleware)
            constraints.emplace_back(name);
        server.registerHandler(
            path,
            [router](drogon::HttpRequestPtr request) {
                return router->dispatch(std::move(request));
            },
            constraints);
        server.registerHandler(
            path + "/{1}",
            [router](drogon::HttpRequestPtr request, std::string id) {
                return router->dispatch(std::move(request), std::move(id));
            },
            constraints);
    }

  protected:
    using Request = drogon::HttpRequest;
    using Items = std::vector<Response>;

    const Service& service() const { return *service_; }

    virtual drogon::Task<Items> list(const Request& request,
                                     CrudPage page) const {
        typename Service::Filters filters;
        for (const auto& [name, value] : request.parameters()) {
            if (name != "page" && name != "per_page")
                Policy::add_filter(filters, name, value);
        }
        return service_->list(page, std::move(filters));
    }

    virtual drogon::Task<std::optional<Response>> get(const Request&,
                                                      Id id) const {
        return service_->get(std::move(id));
    }

    virtual drogon::Task<Response> create(const Request& request) const {
        return service_->create(Policy::parse_create(object_body(request)));
    }

    virtual drogon::Task<std::optional<Response>> patch(const Request& request,
                                                        Id id) const {
        return service_->patch(std::move(id),
                               Policy::parse_patch(object_body(request)));
    }

    virtual drogon::Task<bool> remove(const Request&, Id id) const {
        return service_->remove(std::move(id));
    }

  private:
    static ApiPagination pagination(const Request& request) {
        ApiPagination page{1, 20, false};
        const auto& params = request.parameters();
        if (const auto it = params.find("page"); it != params.end()) {
            page.page = parse_crud_integer<std::int64_t>(it->second);
        }
        if (const auto it = params.find("per_page"); it != params.end()) {
            page.per_page = parse_crud_integer<std::int64_t>(it->second);
        }
        if (page.page < 1 || page.per_page < 1 || page.per_page > 100 ||
            page.page - 1 >
                std::numeric_limits<std::int64_t>::max() / page.per_page) {
            throw CrudValidationError("Invalid pagination.");
        }
        return page;
    }

    static const Json::Value& object_body(const Request& request) {
        const auto& body = request.getJsonObject();
        if (!body || !body->isObject())
            throw CrudValidationError("Expected a JSON object.");
        return *body;
    }

    static Id parse_id(std::string_view value) {
        if constexpr (requires { Policy::parse_id(value); }) {
            return Policy::parse_id(value);
        } else if constexpr (std::integral<Id>) {
            auto id = parse_crud_integer<Id>(value);
            if (id <= 0)
                throw CrudValidationError("Invalid resource ID.");
            return id;
        } else {
            if (value.empty())
                throw CrudValidationError("Invalid resource ID.");
            return Id(value);
        }
    }

    drogon::HttpResponsePtr method_not_allowed(const Request& request,
                                               bool item) const {
        using namespace drogon;
        std::string allow;
        const auto append = [&](bool enabled, std::string_view method) {
            if (!enabled)
                return;
            if (!allow.empty())
                allow += ", ";
            allow += method;
        };
        append(item ? methods_.get : methods_.list, "GET");
        if (item) {
            append(methods_.patch, "PATCH");
            append(methods_.remove, "DELETE");
        } else {
            append(methods_.create, "POST");
        }
        auto response = api_error(request, k405MethodNotAllowed,
                                  "METHOD_NOT_ALLOWED", "Method not allowed.");
        response->addHeader("Allow", std::move(allow));
        return response;
    }

    drogon::Task<drogon::HttpResponsePtr>
    dispatch(drogon::HttpRequestPtr request,
             std::optional<std::string> id = {}) const {
        using namespace drogon;
        try {
            const auto method = request->method();
            if (!id) {
                if (method == Get && methods_.list) {
                    auto page = pagination(*request);
                    auto items = co_await list(
                        *request,
                        {page.per_page + 1, (page.page - 1) * page.per_page});
                    const auto limit = static_cast<std::size_t>(page.per_page);
                    page.has_more = items.size() > limit;
                    if (page.has_more)
                        items.erase(items.begin() + limit, items.end());
                    co_return api_ok(*request, std::move(items), page);
                }
                if (method == Post && methods_.create)
                    co_return api_ok(*request, co_await create(*request),
                                     k201Created);
                co_return method_not_allowed(*request, false);
            }
            if (method == Get && methods_.get) {
                auto item = co_await get(*request, parse_id(*id));
                if (item)
                    co_return api_ok(*request, std::move(*item));
            } else if (method == Patch && methods_.patch) {
                auto item = co_await patch(*request, parse_id(*id));
                if (item)
                    co_return api_ok(*request, std::move(*item));
            } else if (method == Delete && methods_.remove) {
                if (co_await remove(*request, parse_id(*id)))
                    co_return api_ok(*request);
            } else {
                co_return method_not_allowed(*request, true);
            }
            co_return api_error(*request, k404NotFound, "NOT_FOUND",
                                "Resource not found.");
        } catch (const std::exception& error) {
            co_return api_exception(*request, error);
        }
    }

    std::shared_ptr<const Service> service_;
    CrudMethods methods_;
};

}
