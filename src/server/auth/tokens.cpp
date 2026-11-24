#include "app_namespace.hpp"
#include "tokens.hpp"

#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#define JWT_DISABLE_PICOJSON
#include <jwt-cpp/jwt.h>

#include <cstdint>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace {

constexpr auto issuer = "app-template";

struct GlazeTraits {
    using value_type = glz::generic_i64;
    using object_type = value_type::object_t;
    using array_type = value_type::array_t;
    using string_type = std::string;
    using number_type = double;
    using integer_type = std::int64_t;
    using boolean_type = bool;

    static jwt::json::type get_type(const value_type& value) {
        using jwt::json::type;
        if (value.is_object()) return type::object;
        if (value.is_array()) return type::array;
        if (value.is_string()) return type::string;
        if (value.holds<integer_type>()) return type::integer;
        if (value.holds<number_type>()) return type::number;
        if (value.is_boolean()) return type::boolean;
        throw std::invalid_argument("invalid JWT JSON claim type");
    }

    static object_type as_object(const value_type& value) { return value.get<object_type>(); }
    static array_type as_array(const value_type& value) { return value.get<array_type>(); }
    static string_type as_string(const value_type& value) { return value.get<string_type>(); }
    static number_type as_number(const value_type& value) { return value.get<number_type>(); }
    static integer_type as_integer(const value_type& value) { return value.get<integer_type>(); }
    static boolean_type as_boolean(const value_type& value) { return value.get<boolean_type>(); }

    static bool parse(value_type& value, string_type text) {
        return !glz::validate_json(text) && !glz::read_json(value, text);
    }

    static string_type serialize(const value_type& value) {
        auto text = glz::write_json(value);
        if (!text) throw std::runtime_error("could not serialize JWT");
        return std::move(*text);
    }
};

}

std::string Tokens::issue(std::string_view username) const {
    const auto now = std::chrono::floor<std::chrono::seconds>(jwt::date::clock::now());
    return jwt::create<GlazeTraits>()
        .set_type("JWT")
        .set_issuer(issuer)
        .set_subject(std::string(username))
        .set_issued_at(now)
        .set_expires_at(now + lifetime_)
        .sign(jwt::algorithm::hs256{key_});
}

std::optional<std::string> Tokens::verify(std::string_view token) const {
    if (token.empty() || token.size() > 4096) return std::nullopt;
    try {
        const auto decoded = jwt::decode<GlazeTraits>(std::string(token));
        jwt::verify<GlazeTraits>()
            .allow_algorithm(jwt::algorithm::hs256{key_})
            .with_issuer(issuer)
            .verify(decoded);
        if (!decoded.has_subject() || !decoded.has_expires_at() ||
            decoded.get_expires_at() <= jwt::date::clock::now()) {
            return std::nullopt;
        }
        auto username = decoded.get_subject();
        if (username.empty()) return std::nullopt;
        return username;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}
