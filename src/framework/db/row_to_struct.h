#pragma once

#include "foreign_record.h"

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
T read_scalar(const drogon::orm::Field& field) {
    if constexpr (std::is_scoped_enum_v<T>) {
        const auto label = field.as<std::string>();
        const auto value = magic_enum::enum_cast<T>(label);
        if (!value)
            throw std::runtime_error("unknown DAO enum label: " + label);
        return *value;
    } else if constexpr (std::same_as<T, DaoDateTime>) {
        return parse_dao_date_time(field.as<std::string>());
    } else {
        return field.as<T>();
    }
}

template <typename T>
void read_row_field(T& value, const drogon::orm::Field& field, const char* name) {
    if (field.isNull()) {
        throw std::runtime_error(std::string("NULL in required column: ") + name);
    }
    value = read_scalar<T>(field);
}

template <typename T>
void read_row_field(std::optional<T>& value, const drogon::orm::Field& field, const char*) {
    if (field.isNull()) {
        value.reset();
    } else {
        value = read_scalar<T>(field);
    }
}

}

template <glz::reflectable T>
T row_to_struct(const drogon::orm::Row& row, std::span<const std::string> omitted = {}) {
    T result{};
    auto fields = glz::to_tie(result);
    constexpr auto names = glz::reflect<T>::keys;
    auto read_member = [&]<std::size_t index>() {
        const std::string name{names[index]};
        auto& field = glz::get<index>(fields);
        using Traits = detail::FieldTraits<std::remove_cvref_t<decltype(field)>>;
        if constexpr (detail::Nullable<typename Traits::Value>::nullable) {
            if (std::ranges::find(omitted, name) != omitted.end()) {
                if constexpr (Traits::foreign_key().has_value())
                    Traits::set_key(field, std::nullopt);
                else
                    Traits::unwrap(field).reset();
                return;
            }
        }
        if constexpr (Traits::foreign_key().has_value()) {
            typename Traits::Value key{};
            detail::read_row_field(key, row.at(name), name.c_str());
            Traits::set_key(field, std::move(key));
        } else {
            detail::read_row_field(Traits::unwrap(field), row.at(name), name.c_str());
        }
    };
    [&]<std::size_t... index>(std::index_sequence<index...>) {
        (read_member.template operator()<index>(), ...);
    }(std::make_index_sequence<names.size()>{});
    return result;
}

}
