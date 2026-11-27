#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace APP_NAMESPACE {

using ApiErrorFields = std::map<std::string, std::vector<std::string>>;

class RequestValidationError : public std::runtime_error {
  public:
    explicit RequestValidationError(std::string message,
                                    ApiErrorFields fields = {})
        : std::runtime_error(std::move(message)), fields(std::move(fields)) {}

    const ApiErrorFields fields;
};

}
