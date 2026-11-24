#pragma once

#include "app_namespace.hpp"

#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>

#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

template<class Payload>
drogon::HttpResponsePtr jsonResponse(const Payload& payload,
                                     drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    if (auto json = glz::write_json(payload)) {
        response->setStatusCode(status);
        response->setBody(std::move(*json));
    } else {
        response->setStatusCode(drogon::k500InternalServerError);
        response->setBody(R"({"error":"Could not serialize response."})");
    }
    return response;
}

struct JsonError {
    std::string_view error;
};

inline drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode status, std::string_view message) {
    return jsonResponse(JsonError{message}, status);
}

}
