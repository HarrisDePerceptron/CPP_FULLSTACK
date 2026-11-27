#include "app_namespace.h"
#include "user_service.h"
#include "framework/support/random.h"

#include <array>
#include <stdexcept>

namespace APP_NAMESPACE {
namespace {

std::string hash_password(std::string_view password) {
    std::array<char, crypto_pwhash_STRBYTES> hash;
    if (crypto_pwhash_str(hash.data(), password.data(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("could not hash user password");
    }
    return hash.data();
}

}

drogon::Task<std::optional<Credentials>> UserService::seed_demo() const {
    if (co_await dao_.has_demo())
        co_return std::nullopt;

    Credentials credentials{"demo-" + random_hex<6>(), random_hex<16>()};
    if (!co_await dao_.insert_demo(credentials.username, hash_password(credentials.password)))
        co_return std::nullopt;
    co_return credentials;
}

drogon::Task<> UserService::configure_demo(std::string username, std::string password) const {
    co_await dao_.replace_demo(std::move(username), hash_password(password));
}

drogon::Task<std::optional<UserRecord::Id>> UserService::find_id(std::string username) const {
    const auto user = co_await dao_.find_by_username(std::move(username));
    co_return user ? user->id : std::nullopt;
}

drogon::Task<bool> UserService::signup(Credentials credentials, Workers& workers) const {
    auto hash = co_await workers.submit([&] { return hash_password(credentials.password); });
    co_return co_await dao_.insert_account(std::move(credentials.username), std::move(hash));
}

drogon::Task<bool> UserService::authenticate(std::string username, std::string password, Workers& workers) const {
    const auto user = co_await dao_.find_by_username(std::move(username));
    if (!user)
        co_return false;
    co_return co_await workers.submit([&] {
        return crypto_pwhash_str_verify(user->password_hash.c_str(), password.data(), password.size()) == 0;
    });
}

}
