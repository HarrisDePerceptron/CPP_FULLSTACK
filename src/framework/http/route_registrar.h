#pragma once

#include "app_namespace.h"
#include <drogon/HttpAppFramework.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace APP_NAMESPACE {

template <typename... Dependencies> class RouteRegistrar {
  public:
    explicit RouteRegistrar(drogon::HttpAppFramework& server,
                            std::tuple<Dependencies...> dependencies = {})
        : server_(server), dependencies_(std::move(dependencies)) {}

    template <typename... More> auto with(More... more) const {
        return RouteRegistrar<Dependencies..., More...>(
            server_, std::tuple_cat(dependencies_,
                                    std::tuple<More...>(std::move(more)...)));
    }

    template <typename Handler, typename... Middleware>
    void get(const std::string& path, Handler handler,
             Middleware... middleware) const {
        server_.registerHandler(
            path, this->bind(handler),
            {drogon::Get,
             RouteRegistrar::middleware_constraint(middleware)...});
    }

    template <typename Handler, typename... Middleware>
    void post(const std::string& path, Handler handler,
              Middleware... middleware) const {
        server_.registerHandler(
            path, this->bind(handler),
            {drogon::Post,
             RouteRegistrar::middleware_constraint(middleware)...});
    }

    template <typename Handler, typename... Middleware>
    void put(const std::string& path, Handler handler,
             Middleware... middleware) const {
        server_.registerHandler(
            path, this->bind(handler),
            {drogon::Put,
             RouteRegistrar::middleware_constraint(middleware)...});
    }

    template <typename Handler, typename... Middleware>
    void patch(const std::string& path, Handler handler,
               Middleware... middleware) const {
        server_.registerHandler(
            path, this->bind(handler),
            {drogon::Patch,
             RouteRegistrar::middleware_constraint(middleware)...});
    }

    template <typename Handler, typename... Middleware>
    void del(const std::string& path, Handler handler,
             Middleware... middleware) const {
        server_.registerHandler(
            path, this->bind(handler),
            {drogon::Delete,
             RouteRegistrar::middleware_constraint(middleware)...});
    }

  private:
    static drogon::internal::HttpConstraint
    middleware_constraint(drogon::internal::HttpConstraint constraint) {
        if (constraint.type() !=
            drogon::internal::ConstraintType::HttpMiddleware)
            throw std::invalid_argument(
                "Expected route middleware after the handler");
        return constraint;
    }

    template <typename Result, typename... Args>
        requires(sizeof...(Args) > sizeof...(Dependencies))
    auto bind(Result (*handler)(Args...)) const {
        constexpr auto count = sizeof...(Dependencies);
        using Arguments = std::tuple<Args...>;
        static_assert(std::is_same_v<std::tuple_element_t<count, Arguments>,
                                     drogon::HttpRequestPtr>,
                      "Expected bound dependencies followed by HttpRequestPtr "
                      "and path arguments");
        auto bound = std::apply(
            [handler](const auto&... dependencies) {
                return std::bind_front(handler, dependencies...);
            },
            dependencies_);

        // Drogon inspects the callable signature to bind URL parameters.
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return [bound = std::move(bound)](
                       drogon::HttpRequestPtr request,
                       std::tuple_element_t<count + 1 + I,
                                            Arguments>... path_args) {
                return bound(std::move(request), std::move(path_args)...);
            };
        }(std::make_index_sequence<sizeof...(Args) - count - 1>{});
    }

    drogon::HttpAppFramework& server_;
    std::tuple<Dependencies...> dependencies_;
};

}
