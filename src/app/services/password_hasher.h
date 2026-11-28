#pragma once

#include "app_namespace.h"
#include <drogon/utils/coroutine.h>
#include <functional>
#include <string>

namespace APP_NAMESPACE {

class Workers;

class PasswordHasher {
  public:
    explicit PasswordHasher(Workers& workers) : workers_(workers) {}

    drogon::Task<std::string> hash(std::string password) const;
    drogon::Task<bool> verify(std::string password, std::string encoded) const;

  private:
    std::reference_wrapper<Workers> workers_;
};

}
