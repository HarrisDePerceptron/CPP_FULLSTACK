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
    drogon::sync_wait(dao_.initialize());
    if (drogon::sync_wait(dao_.hasDemo())) return std::nullopt;

    Credentials credentials{"demo-" + randomHex<6>(), randomHex<16>()};
    if (!drogon::sync_wait(dao_.insertDemo(credentials.username, hashPassword(credentials.password)))) return std::nullopt;
    return credentials;
}

void UserService::configureDemo(std::string_view username, std::string_view password) const {
    drogon::sync_wait(dao_.initialize());
    drogon::sync_wait(dao_.replaceDemo(std::string(username), hashPassword(password)));
}

drogon::Task<bool> UserService::authenticate(std::string username, std::string password, Workers& workers) const {
    const auto user = co_await dao_.findById(std::move(username));
    if (!user) co_return false;
    co_return co_await workers.submit([&] {
        return crypto_pwhash_str_verify(user->passwordHash.c_str(), password.data(), password.size()) == 0;
    });
}

}
