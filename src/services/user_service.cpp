#include "app_namespace.hpp"
#include "user_service.hpp"
#include "utils/random.hpp"

#include <array>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace {

std::string hashPassword(std::string_view password) {
    std::array<char, crypto_pwhash_STRBYTES> hash;
    if (crypto_pwhash_str(hash.data(), password.data(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("could not hash demo user password");
    }
    return hash.data();
}

}


std::optional<Credentials> UserService::seedDemo() const {
    dao_.initialize();
    if (dao_.hasDemo()) return std::nullopt;

    Credentials credentials{"demo-" + randomHex<6>(), randomHex<16>()};
    if (!dao_.insertDemo(credentials.username, hashPassword(credentials.password))) return std::nullopt;
    return credentials;
}

void UserService::configureDemo(std::string_view username, std::string_view password) const {
    dao_.initialize();
    dao_.replaceDemo(username, hashPassword(password));
}

bool UserService::authenticate(std::string_view username, std::string_view password) const {
    const auto hash = dao_.passwordHash(username);
    return hash && crypto_pwhash_str_verify(hash->c_str(), password.data(), password.size()) == 0;
}

}
