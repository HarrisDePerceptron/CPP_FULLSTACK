#pragma once

#include "schema_field.hpp"

#include <drogon/orm/Row.h>
#include <drogon/orm/Field.h>
#include <glaze/core/reflect.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace APP_NAMESPACE {

namespace detail {

template <typename T>
void readRowField(T& value, const drogon::orm::Field& field, const char* name) {
    if (field.isNull()) {
        throw std::runtime_error(std::string("NULL in required column: ") + name);
    }
    value = field.as<T>();
}

template <typename T>
void readRowField(std::optional<T>& value, const drogon::orm::Field& field, const char*) {
    if (field.isNull()) {
        value.reset();
    } else {
        value = field.as<T>();
    }
}

}

template <glz::reflectable T>
T rowToStruct(const drogon::orm::Row& row) {
    T result{};
    auto fields = glz::to_tie(result);
    constexpr auto names = glz::reflect<T>::keys;
    auto readMember = [&]<std::size_t I>() {
        const std::string name{names[I]};
        auto& field = glz::get<I>(fields);
        using Traits = detail::FieldTraits<std::remove_cvref_t<decltype(field)>>;
        detail::readRowField(Traits::unwrap(field), row.at(name), name.c_str());
    };
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (readMember.template operator()<I>(), ...);
    }(std::make_index_sequence<names.size()>{});
    return result;
}

}
