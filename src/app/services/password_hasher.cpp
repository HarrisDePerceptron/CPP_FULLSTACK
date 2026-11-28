#include "password_hasher.h"
#include "framework/support/workers.h"

#include <sodium.h>
#include <array>
#include <stdexcept>

namespace APP_NAMESPACE {

drogon::Task<std::string> PasswordHasher::hash(std::string password) const {
    co_return co_await workers_.get().run_blocking([&password] {
        std::array<char, crypto_pwhash_STRBYTES> encoded;
        if (crypto_pwhash_str(encoded.data(), password.data(), password.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
            throw std::runtime_error("could not hash user password");
        return std::string(encoded.data());
    });
}

drogon::Task<bool> PasswordHasher::verify(std::string password,
                                          std::string encoded) const {
    co_return co_await workers_.get().run_blocking([&password, &encoded] {
        return crypto_pwhash_str_verify(encoded.c_str(), password.data(),
                                        password.size()) == 0;
    });
}

}
