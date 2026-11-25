#pragma once

#include "server/json_response.hpp"
#include "services/crud_service.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/Exception.h>

#include <charconv>
#include <concepts>

namespace APP_NAMESPACE {

struct CrudMethods {
    bool list = true;
    bool get = true;
    bool create = true;
    bool patch = true;
    bool remove = true;
};

template <std::integral Integer>
Integer parseCrudInteger(std::string_view text) {
    if (text.empty()) throw CrudValidationError("Expected an integer.");
    Integer value{};
    const auto end = text.data() + text.size();
    const auto [next, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || next != end) throw CrudValidationError("Expected an integer.");
    return value;
}

template <typename Service>
class CrudRouter {
public:
    using Response = typename Service::Response;
    using Id = typename Service::Id;
    using Policy = typename Service::Policy;

    explicit CrudRouter(std::shared_ptr<const Service> service, CrudMethods methods = {})
        : service_(std::move(service)), methods_(methods) {
        if (!service_) throw std::invalid_argument("CRUD router requires a service");
    }
    virtual ~CrudRouter() = default;

    static void mount(drogon::HttpAppFramework& server, std::string path,
                      std::shared_ptr<const CrudRouter> router,
                      const std::vector<std::string>& middleware = {}) {
        if (!router) throw std::invalid_argument("CRUD mount requires a router");
        std::vector<drogon::internal::HttpConstraint> constraints;
        for (const auto& name : middleware) constraints.emplace_back(name);
        server.registerHandler(path, [router](drogon::HttpRequestPtr request) {
            return router->dispatch(std::move(request));
        }, constraints);
        server.registerHandler(path + "/{1}", [router](drogon::HttpRequestPtr request, std::string id) {
            return router->dispatch(std::move(request), std::move(id));
        }, constraints);
    }

protected:
    using Request = drogon::HttpRequest;
    using Items = std::vector<Response>;

    const Service& service() const { return *service_; }

    virtual drogon::Task<Items> list(const Request& request) const {
        CrudPage page;
        typename Service::Filters filters;
        for (const auto& [name, value] : request.parameters()) {
            if (name == "limit") page.limit = parseCrudInteger<std::int64_t>(value);
            else if (name == "offset") page.offset = parseCrudInteger<std::int64_t>(value);
            else Policy::addFilter(filters, name, value);
        }
        if (page.limit < 1 || page.limit > 100 || page.offset < 0) {
            throw CrudValidationError("Invalid pagination.");
        }
        return service_->list(page, std::move(filters));
    }

    virtual drogon::Task<std::optional<Response>> get(const Request&, Id id) const {
        return service_->get(std::move(id));
    }

    virtual drogon::Task<Response> create(const Request& request) const {
        return service_->create(Policy::parseCreate(objectBody(request)));
    }

    virtual drogon::Task<std::optional<Response>> patch(const Request& request, Id id) const {
        return service_->patch(std::move(id), Policy::parsePatch(objectBody(request)));
    }

    virtual drogon::Task<bool> remove(const Request&, Id id) const {
        return service_->remove(std::move(id));
    }

private:
    struct Envelope {
        Items result;
        std::string error;
        std::string message;
    };

    static drogon::HttpResponsePtr reply(drogon::HttpStatusCode status, Items items = {},
                                         std::string error = {}) {
        auto response = jsonResponse(Envelope{std::move(items), std::move(error), ""}, status);
        if (status != drogon::k500InternalServerError &&
            response->statusCode() == drogon::k500InternalServerError) {
            return reply(drogon::k500InternalServerError, {}, "Internal server error.");
        }
        return response;
    }

    static drogon::HttpResponsePtr single(drogon::HttpStatusCode status, Response item) {
        Items items;
        items.push_back(std::move(item));
        return reply(status, std::move(items));
    }

    static const Json::Value& objectBody(const Request& request) {
        const auto& body = request.getJsonObject();
        if (!body || !body->isObject()) throw CrudValidationError("Expected a JSON object.");
        return *body;
    }

    static Id parseId(std::string_view value) {
        if constexpr (requires { Policy::parseId(value); }) {
            return Policy::parseId(value);
        } else if constexpr (std::integral<Id>) {
            auto id = parseCrudInteger<Id>(value);
            if (id <= 0) throw CrudValidationError("Invalid resource ID.");
            return id;
        } else {
            if (value.empty()) throw CrudValidationError("Invalid resource ID.");
            return Id(value);
        }
    }

    drogon::HttpResponsePtr methodNotAllowed(bool item) const {
        using namespace drogon;
        std::string allow;
        const auto append = [&](bool enabled, std::string_view method) {
            if (!enabled) return;
            if (!allow.empty()) allow += ", ";
            allow += method;
        };
        append(item ? methods_.get : methods_.list, "GET");
        if (item) {
            append(methods_.patch, "PATCH");
            append(methods_.remove, "DELETE");
        } else {
            append(methods_.create, "POST");
        }
        auto response = reply(k405MethodNotAllowed, {}, "Method not allowed.");
        response->addHeader("Allow", std::move(allow));
        return response;
    }

    drogon::Task<drogon::HttpResponsePtr> dispatch(drogon::HttpRequestPtr request,
                                                    std::optional<std::string> id = {}) const {
        using namespace drogon;
        try {
            const auto method = request->method();
            if (!id) {
                if (method == Get && methods_.list) co_return reply(k200OK, co_await list(*request));
                if (method == Post && methods_.create) co_return single(k201Created, co_await create(*request));
                co_return methodNotAllowed(false);
            }
            if (method == Get && methods_.get) {
                auto item = co_await get(*request, parseId(*id));
                if (item) co_return single(k200OK, std::move(*item));
            } else if (method == Patch && methods_.patch) {
                auto item = co_await patch(*request, parseId(*id));
                if (item) co_return single(k200OK, std::move(*item));
            } else if (method == Delete && methods_.remove) {
                if (co_await remove(*request, parseId(*id))) co_return reply(k200OK);
            } else {
                co_return methodNotAllowed(true);
            }
            co_return reply(k404NotFound, {}, "Resource not found.");
        } catch (const CrudValidationError& error) {
            co_return reply(k400BadRequest, {}, error.what());
        } catch (const orm::SqlError& error) {
            constexpr int sqliteConstraint = 19;
            if ((error.errcode() & 0xff) == sqliteConstraint ||
                (error.extendedErrcode() & 0xff) == sqliteConstraint ||
                error.sqlState().starts_with("23")) {
                co_return reply(k409Conflict, {}, "Resource conflicts with a database constraint.");
            }
        } catch (const std::exception&) {
        }
        co_return reply(k500InternalServerError, {}, "Internal server error.");
    }

    std::shared_ptr<const Service> service_;
    CrudMethods methods_;
};

}
