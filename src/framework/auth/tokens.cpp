#include "app_namespace.h"
#include "tokens.h"

#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#define JWT_DISABLE_PICOJSON
#include <jwt-cpp/jwt.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace {

constexpr auto token_issuer = "app-template";

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
        if (value.is_object())
            return type::object;
        if (value.is_array())
            return type::array;
        if (value.is_string())
            return type::string;
        if (value.holds<integer_type>())
            return type::integer;
        if (value.holds<number_type>())
            return type::number;
        if (value.is_boolean())
            return type::boolean;
        throw std::invalid_argument("invalid JWT JSON claim type");
    }

    static object_type as_object(const value_type& value) {
        return value.get<object_type>();
    }
    static array_type as_array(const value_type& value) {
        return value.get<array_type>();
    }
    static string_type as_string(const value_type& value) {
        return value.get<string_type>();
    }
    static number_type as_number(const value_type& value) {
        return value.get<number_type>();
    }
    static integer_type as_integer(const value_type& value) {
        return value.get<integer_type>();
    }
    static boolean_type as_boolean(const value_type& value) {
        return value.get<boolean_type>();
    }

    static bool parse(value_type& value, string_type text) {
        return !glz::validate_json(text) && !glz::read_json(value, text);
    }

    static string_type serialize(const value_type& value) {
        auto text = glz::write_json(value);
        if (!text)
            throw std::runtime_error("could not serialize JWT");
        return std::move(*text);
    }
};

}

std::string load_signing_key() {
    std::array<unsigned char, 32> key;
    if (const auto* configured = std::getenv("APP_JWT_SECRET")) {
        const std::string_view hex(configured);
        if (hex.size() != key.size() * 2 ||
            sodium_hex2bin(key.data(), key.size(), hex.data(), hex.size(),
                           nullptr, nullptr, nullptr) != 0) {
            throw std::runtime_error(
                "APP_JWT_SECRET must contain 64 hexadecimal characters");
        }
    } else {
        randombytes_buf(key.data(), key.size());
    }
    std::string result(reinterpret_cast<const char*>(key.data()), key.size());
    sodium_memzero(key.data(), key.size());
    return result;
}

std::string Tokens::issue(std::string_view username,
                          std::span<const std::string> roles) const {
    const auto now =
        std::chrono::floor<std::chrono::seconds>(jwt::date::clock::now());
    GlazeTraits::array_type role_claim;
    role_claim.reserve(roles.size());
    for (const auto& role : roles) {
        if (role.empty())
            throw std::invalid_argument("JWT role names must be nonempty");
        role_claim.emplace_back(role);
    }
    return jwt::create<GlazeTraits>()
        .set_type("JWT")
        .set_issuer(token_issuer)
        .set_subject(std::string(username))
        .set_issued_at(now)
        .set_expires_at(now + lifetime_)
        .set_payload_claim("roles",
                           GlazeTraits::value_type(std::move(role_claim)))
        .sign(jwt::algorithm::hs256{key_});
}

std::optional<VerifiedJwt> Tokens::verify(std::string_view token) const {
    if (token.empty() || token.size() > 4096)
        return std::nullopt;
    try {
        const auto decoded = jwt::decode<GlazeTraits>(std::string(token));
        jwt::verify<GlazeTraits>()
            .allow_algorithm(jwt::algorithm::hs256{key_})
            .with_issuer(token_issuer)
            .verify(decoded);
        if (!decoded.has_subject() || !decoded.has_expires_at() ||
            decoded.get_expires_at() <= jwt::date::clock::now()) {
            return std::nullopt;
        }
        auto username = decoded.get_subject();
        if (username.empty())
            return std::nullopt;
        auto claims = decoded.get_payload_json();
        std::vector<std::string> roles;
        if (const auto it = claims.find("roles"); it != claims.end()) {
            const auto& claim = it->second;
            if (!claim.is_array())
                return std::nullopt;
            for (const auto& entry : claim.get<GlazeTraits::array_type>()) {
                if (!entry.is_string() || entry.get<std::string>().empty())
                    return std::nullopt;
                roles.push_back(entry.get<std::string>());
            }
            std::ranges::sort(roles);
            const auto duplicates = std::ranges::unique(roles);
            roles.erase(duplicates.begin(), duplicates.end());
        }
        std::optional<std::chrono::system_clock::time_point> issued_at;
        if (decoded.has_issued_at())
            issued_at = decoded.get_issued_at();
        return VerifiedJwt{std::move(username),       decoded.get_issuer(),
                           decoded.get_expires_at(),  issued_at,
                           decoded.get_header_json(), std::move(claims),
                           std::move(roles)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}
