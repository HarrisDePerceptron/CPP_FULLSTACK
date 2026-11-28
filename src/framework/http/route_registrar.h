#pragma once

#include "route_paths.h"

#include <drogon/HttpAppFramework.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace APP_NAMESPACE {

template <typename Service> class CrudRouter;

namespace detail {

struct RouteDefinition {
    drogon::HttpMethod method;
    std::string path;
    std::function<void(drogon::HttpAppFramework&, const std::string&)> install;

    RouteMatch match() const { return {method, path}; }
};

class RouteTable {
  public:
    explicit RouteTable(drogon::HttpAppFramework& server) : server_(server) {}
    void install(const std::vector<RouteDefinition>& routes);

  private:
    struct InstalledRoute {
        drogon::HttpMethod method;
        std::string path;

        RouteMatch match() const { return {method, path}; }
    };

    drogon::HttpAppFramework& server_;
    std::vector<InstalledRoute> installed_;
};

template <typename Call, typename... Args>
    requires(sizeof...(Args) > 0)
auto with_handler_last(Call call, Args... args) {
    auto arguments = std::tuple<Args...>(std::move(args)...);
    return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        return call(std::get<sizeof...(Args) - 1>(std::move(arguments)),
                    std::get<Index>(std::move(arguments))...);
    }(std::make_index_sequence<sizeof...(Args) - 1>{});
}

inline drogon::internal::HttpConstraint
middleware_constraint(drogon::internal::HttpConstraint constraint) {
    if (constraint.type() != drogon::internal::ConstraintType::HttpMiddleware)
        throw std::invalid_argument(
            "Expected route middleware before the handler");
    return constraint;
}

}

template <RoutePath Prefix = RoutePath{""}, typename Bindings = std::tuple<>,
          typename... Keys>
class RouteRegistrar {
  public:
    explicit RouteRegistrar(detail::RouteTable& table) : table_(table) {
        static_assert(detail::valid_route_prefix(Prefix.view()),
                      "Invalid route prefix");
    }

    template <typename... More> [[nodiscard]] auto with(More... more) const {
        auto bindings = std::tuple_cat(dependencies_,
                                       std::tuple<More...>(std::move(more)...));
        return RouteRegistrar<Prefix, decltype(bindings), Keys...>(
            table_, std::move(bindings), definitions_);
    }

    template <RoutePath OtherPrefix, typename OtherBindings,
              typename... OtherKeys>
        requires(detail::RouteKeys<Keys...>::template can_merge<OtherKeys...>)
    auto
    merge(const RouteRegistrar<OtherPrefix, OtherBindings, OtherKeys...>& other)
        const {
        if (&table_ != &other.table_)
            throw std::invalid_argument(
                "Route groups must use the same runtime");
        auto definitions = definitions_;
        definitions.insert(definitions.end(), other.definitions_.begin(),
                           other.definitions_.end());
        return RouteRegistrar<Prefix, Bindings, Keys..., OtherKeys...>(
            table_, dependencies_, std::move(definitions));
    }

    void build() const { table_.install(definitions_); }

    template <RoutePath Path, typename... Args>
        requires(detail::DistinctRoute<
                 detail::RouteKey<drogon::Get, Prefix, Path>, Keys...>)
    [[nodiscard]] auto get(Args... args) const {
        return this->template add_bound<drogon::Get, Path>(std::move(args)...);
    }

    template <RoutePath Path, typename... Args>
        requires(detail::DistinctRoute<
                 detail::RouteKey<drogon::Post, Prefix, Path>, Keys...>)
    [[nodiscard]] auto post(Args... args) const {
        return this->template add_bound<drogon::Post, Path>(std::move(args)...);
    }

    template <RoutePath Path, typename... Args>
        requires(detail::DistinctRoute<
                 detail::RouteKey<drogon::Put, Prefix, Path>, Keys...>)
    [[nodiscard]] auto put(Args... args) const {
        return this->template add_bound<drogon::Put, Path>(std::move(args)...);
    }

    template <RoutePath Path, typename... Args>
        requires(detail::DistinctRoute<
                 detail::RouteKey<drogon::Patch, Prefix, Path>, Keys...>)
    [[nodiscard]] auto patch(Args... args) const {
        return this->template add_bound<drogon::Patch, Path>(
            std::move(args)...);
    }

    template <RoutePath Path, typename... Args>
        requires(detail::DistinctRoute<
                 detail::RouteKey<drogon::Delete, Prefix, Path>, Keys...>)
    [[nodiscard]] auto del(Args... args) const {
        return this->template add_bound<drogon::Delete, Path>(
            std::move(args)...);
    }

  private:
    template <RoutePath, typename, typename...> friend class RouteRegistrar;
    template <typename> friend class CrudRouter;

    RouteRegistrar(detail::RouteTable& table, Bindings dependencies,
                   std::vector<detail::RouteDefinition> definitions)
        : table_(table), dependencies_(std::move(dependencies)),
          definitions_(std::move(definitions)) {}

    template <drogon::HttpMethod Method, RoutePath Path, typename... Args>
    auto add_bound(Args... args) const {
        return detail::with_handler_last(
            [this](auto handler, auto... middleware) {
                return this->template add<Method, Path>(this->bind(handler),
                                                        middleware...);
            },
            std::move(args)...);
    }

    template <drogon::HttpMethod Method, RoutePath Path, typename Handler,
              typename... Middleware>
        requires(detail::DistinctRoute<detail::RouteKey<Method, Prefix, Path>,
                                       Keys...>)
    auto add(Handler handler, Middleware... middleware) const {
        using Key = detail::RouteKey<Method, Prefix, Path>;
        static_assert(detail::valid_route_path(Key::match.path),
                      "Invalid route path");
        std::vector<drogon::internal::HttpConstraint> constraints;
        if constexpr (Method != detail::all_route_methods)
            constraints.emplace_back(Method);
        (constraints.push_back(detail::middleware_constraint(middleware)), ...);
        auto definitions = definitions_;
        definitions.push_back(
            {Method, std::string(Key::match.path),
             [handler = std::move(handler),
              constraints = std::move(constraints)](
                 drogon::HttpAppFramework& server, const std::string& path) {
                 // Pass Drogon an owned callable, independent of the registrar.
                 server.registerHandler(path, Handler(handler), constraints);
             }});
        return RouteRegistrar<Prefix, Bindings, Keys..., Key>(
            table_, dependencies_, std::move(definitions));
    }

    template <typename Result, typename... Args>
        requires(sizeof...(Args) > std::tuple_size_v<Bindings>)
    auto bind(Result (*handler)(Args...)) const {
        constexpr auto count = std::tuple_size_v<Bindings>;
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

    detail::RouteTable& table_;
    Bindings dependencies_{};
    std::vector<detail::RouteDefinition> definitions_;
};

}
