#pragma once

#include "schema_field.hpp"

#include <glaze/core/reflect.hpp>

#include <chrono>
#include <cstdint>
#include <tuple>

namespace APP_NAMESPACE {

using DaoDateTime = std::chrono::sys_time<std::chrono::milliseconds>;

namespace detail {

template <typename Fields, std::size_t I>
using DaoFieldTraits = FieldTraits<std::remove_cvref_t<decltype(
    glz::get<I>(glz::to_tie(std::declval<Fields&>())))>>;

template <typename Fields, std::size_t I>
constexpr auto primaryType() {
    using Traits = DaoFieldTraits<Fields, I>;
    if constexpr (Traits::primary) {
        static_assert(!Nullable<typename Traits::Value>::nullable, "primary key cannot be nullable");
        return std::tuple{std::type_identity<typename Traits::Value>{}};
    } else {
        return std::tuple{};
    }
}

template <typename Fields, std::size_t... I>
constexpr auto primaryTypes(std::index_sequence<I...>) {
    return std::tuple_cat(primaryType<Fields, I>()...);
}

template <typename Fields>
using PrimaryTypes = decltype(primaryTypes<Fields>(
    std::make_index_sequence<glz::reflect<Fields>::keys.size()>{}));

template <typename Fields>
struct DaoTimestamps {
    std::optional<DaoDateTime> created_at, updated_at;
};

template <typename Fields, bool Generated>
struct DaoGeneratedKey {};

template <typename Fields>
struct DaoGeneratedKey<Fields, true> {
    std::optional<std::int64_t> id;
};

DaoDateTime parseDaoDateTime(std::string_view text);
std::string formatDaoDateTime(DaoDateTime value);

}

template <glz::reflectable Fields>
struct DaoRecord : Fields, detail::DaoTimestamps<Fields>,
                   detail::DaoGeneratedKey<Fields, std::tuple_size_v<detail::PrimaryTypes<Fields>> == 0> {
    using FieldRecord = Fields;
    using Keys = detail::PrimaryTypes<Fields>;
    static_assert(std::tuple_size_v<Keys> <= 1, "only one primary field is supported");
    static constexpr bool generatedId = std::tuple_size_v<Keys> == 0;

    using Id = typename std::tuple_element_t<0, decltype(std::tuple_cat(
        Keys{}, std::tuple{std::type_identity<std::int64_t>{}}))>::type;

    static constexpr std::string_view keyColumn = []<std::size_t... I>(std::index_sequence<I...>) {
        std::string_view key = "id";
        constexpr auto names = glz::reflect<Fields>::keys;
        ((detail::DaoFieldTraits<Fields, I>::primary ? void(key = names[I]) : void()), ...);
        return key;
    }(std::make_index_sequence<glz::reflect<Fields>::keys.size()>{});

    DaoRecord() = default;
    DaoRecord(Fields fields) : Fields(std::move(fields)) {}
};

}
