#pragma once

#include "foreign_record.hpp"

#include <drogon/orm/Row.h>
#include <drogon/orm/Field.h>
#include <glaze/core/reflect.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace APP_NAMESPACE {

namespace detail {

template <typename T>
T readScalar(const drogon::orm::Field& field) {
    if constexpr (std::is_scoped_enum_v<T>) {
        const auto label = field.as<std::string>();
        const auto value = magic_enum::enum_cast<T>(label);
        if (!value) throw std::runtime_error("unknown DAO enum label: " + label);
        return *value;
    } else if constexpr (std::same_as<T, DaoDateTime>) {
        return parseDaoDateTime(field.as<std::string>());
    } else {
        return field.as<T>();
    }
}

template <typename T>
void readRowField(T& value, const drogon::orm::Field& field, const char* name) {
    if (field.isNull()) {
        throw std::runtime_error(std::string("NULL in required column: ") + name);
    }
    value = readScalar<T>(field);
}

template <typename T>
void readRowField(std::optional<T>& value, const drogon::orm::Field& field, const char*) {
    if (field.isNull()) {
        value.reset();
    } else {
        value = readScalar<T>(field);
    }
}

}

template <glz::reflectable T>
T rowToStruct(const drogon::orm::Row& row, std::span<const std::string> omitted = {}) {
    T result{};
    auto fields = glz::to_tie(result);
    constexpr auto names = glz::reflect<T>::keys;
    auto readMember = [&]<std::size_t I>() {
        const std::string name{names[I]};
        auto& field = glz::get<I>(fields);
        using Traits = detail::FieldTraits<std::remove_cvref_t<decltype(field)>>;
        if constexpr (detail::Nullable<typename Traits::Value>::nullable) {
            if (std::ranges::find(omitted, name) != omitted.end()) {
                if constexpr (Traits::foreignKey().has_value()) Traits::setKey(field, std::nullopt);
                else Traits::unwrap(field).reset();
                return;
            }
        }
        if constexpr (Traits::foreignKey().has_value()) {
            typename Traits::Value key{};
            detail::readRowField(key, row.at(name), name.c_str());
            Traits::setKey(field, std::move(key));
        } else {
            detail::readRowField(Traits::unwrap(field), row.at(name), name.c_str());
        }
    };
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (readMember.template operator()<I>(), ...);
    }(std::make_index_sequence<names.size()>{});
    return result;
}

}
