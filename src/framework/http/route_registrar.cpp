#include "route_registrar.h"

#include <stdexcept>

namespace APP_NAMESPACE::detail {

void RouteTable::install(const std::vector<RouteDefinition>& routes) {
    auto pending = installed_;
    for (const auto& route : routes) {
        for (const auto& existing : pending) {
            if (routes_clash(route.match(), existing.match()))
                throw std::invalid_argument("Route clash: " + route.path +
                                            " and " + existing.path);
        }
        pending.push_back({route.method, route.path});
    }

    for (const auto& route : routes)
        route.install(server_, route.path);
    installed_.swap(pending);
}

}
