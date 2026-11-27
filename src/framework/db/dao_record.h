#pragma once

#include "schema_field.h"

#include <glaze/core/reflect.hpp>

#include <algorithm>
#include <cstdint>
#include <tuple>

namespace APP_NAMESPACE {

namespace detail {

template <typename Fields, std::size_t index>
using DaoFieldTraits = FieldTraits<std::remove_cvref_t<decltype(glz::get<index>(glz::to_tie(std::declval<Fields&>())))>>;

template <typename Fields, std::size_t index>
constexpr auto primary_type() {
    using Traits = DaoFieldTraits<Fields, index>;
    if constexpr (Traits::primary) {
        static_assert(!Nullable<typename Traits::Value>::nullable, "primary key cannot be nullable");
        return std::tuple{std::type_identity<typename Traits::Value>{}};
    } else {
        return std::tuple{};
    }
}

template <typename Fields, std::size_t... index>
constexpr auto primary_types(std::index_sequence<index...>) {
    return std::tuple_cat(primary_type<Fields, index>()...);
}

template <typename Fields>
using PrimaryTypes = decltype(primary_types<Fields>(
    std::make_index_sequence<glz::reflect<Fields>::keys.size()>{}));

template <typename Fields>
struct DaoTimestamps {
    std::optional<DaoDateTime> created_at, updated_at;
};

template <typename Fields, bool generated>
struct DaoGeneratedKey {};

struct DaoDeletion {
    std::optional<DaoDateTime> deleted_at;
};

template <typename Fields>
struct DaoGeneratedKey<Fields, true> {
    std::optional<std::int64_t> id;
};

}

template <glz::reflectable Fields>
struct DaoRecord : Fields, detail::DaoTimestamps<Fields>, detail::DaoDeletion, detail::DaoGeneratedKey<Fields, std::tuple_size_v<detail::PrimaryTypes<Fields>> == 0> {
    using FieldRecord = Fields;
    using Keys = detail::PrimaryTypes<Fields>;
    static_assert(std::tuple_size_v<Keys> <= 1, "only one primary field is supported");
    static constexpr bool generated_id = std::tuple_size_v<Keys> == 0;

    using Id = typename std::tuple_element_t<0, decltype(std::tuple_cat(
                                                    Keys{}, std::tuple{std::type_identity<std::int64_t>{}}))>::type;

    static constexpr std::string_view key_column = []<std::size_t... index>(std::index_sequence<index...>) {
        std::string_view key = "id";
        constexpr auto names = glz::reflect<Fields>::keys;
        ((detail::DaoFieldTraits<Fields, index>::primary ? void(key = names[index]) : void()), ...);
        return key;
    }(std::make_index_sequence<glz::reflect<Fields>::keys.size()>{});

    DaoRecord() = default;
    DaoRecord(Fields fields) : Fields(std::move(fields)) {}
    DaoRecord(Fields fields, std::vector<std::string> omitted)
        : Fields(std::move(fields)), omitted_(std::move(omitted)) {}

    bool is_column_loaded(std::string_view name) const {
        return std::ranges::find(omitted_, name) == omitted_.end();
    }

    template <auto member>
        requires std::is_member_object_pointer_v<decltype(member)> &&
                 requires(DaoRecord& record) { record.*member; }
    bool is_loaded() const {
        return is_column_loaded(glz::get_name<member>());
    }

  private:
    std::vector<std::string> omitted_;
};

}
