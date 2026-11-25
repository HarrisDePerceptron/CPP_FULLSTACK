#pragma once

#include "app_namespace.h"
#include "utils/random.h"
#include "utils/request_validation.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>

#include <ctime>

namespace APP_NAMESPACE {

struct ApiPagination {
    std::int64_t page = 1;
    std::int64_t per_page = 1;
    bool has_more = false;
};

struct ApiMeta {
    std::string timestamp;
    std::string request_id;
    std::optional<ApiPagination> pagination;
};

struct ApiError {
    std::string code;
    std::string message;
    std::optional<ApiErrorFields> fields;
};

template <typename T>
struct ApiResponse {
    bool success;
    std::optional<std::vector<T>> data;
    std::optional<ApiError> error;
    ApiMeta meta;
};

inline std::string api_request_id(const drogon::HttpRequest& request) {
    constexpr auto key = "app.requestId";
    const auto attributes = request.attributes();
    auto id = attributes->get<std::string>(key);
    if (id.empty()) {
        id = "req_" + random_hex<16>();
        attributes->insert(key, id);
    }
    return id;
}

inline ApiMeta api_meta(const drogon::HttpRequest& request, std::optional<ApiPagination> pagination) {
    const auto now = std::time(nullptr);
    std::tm utc{};
    char timestamp[21]{};
    if (gmtime_r(&now, &utc)) std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return {timestamp, api_request_id(request), pagination};
}

template <typename T>
drogon::HttpResponsePtr serialize_api(const ApiResponse<T>& payload, drogon::HttpStatusCode status) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->addHeader("X-Request-ID", payload.meta.request_id);
    if (auto json = glz::write_json(payload)) {
        response->setStatusCode(status);
        response->setBody(std::move(*json));
    } else {
        response->setStatusCode(drogon::k500InternalServerError);
        auto meta = payload.meta;
        meta.pagination.reset();
        auto fallback = glz::write_json(ApiResponse<bool>{
            false, {}, ApiError{"INTERNAL_ERROR", "Internal server error.", {}}, std::move(meta)});
        response->setBody(std::move(fallback).value());
    }
    return response;
}

template <typename T>
drogon::HttpResponsePtr api_ok(const drogon::HttpRequest& request, std::vector<T> items,
                            ApiPagination pagination, drogon::HttpStatusCode status = drogon::k200OK) {
    return serialize_api(ApiResponse<T>{true, std::move(items), {}, api_meta(request, pagination)}, status);
}

template <typename T>
drogon::HttpResponsePtr api_ok(const drogon::HttpRequest& request, T item,
                            drogon::HttpStatusCode status = drogon::k200OK) {
    std::vector<T> items;
    items.push_back(std::move(item));
    return api_ok(request, std::move(items), {}, status);
}

inline drogon::HttpResponsePtr api_ok(const drogon::HttpRequest& request) {
    return api_ok(request, std::vector<int>{}, ApiPagination{});
}

inline drogon::HttpResponsePtr api_error(const drogon::HttpRequest& request, drogon::HttpStatusCode status,
                                       std::string code, std::string message, ApiErrorFields fields = {}) {
    ApiError error{std::move(code), std::move(message), {}};
    if (!fields.empty()) error.fields.emplace(std::move(fields));
    return serialize_api(ApiResponse<bool>{false, {}, std::move(error), api_meta(request, {})}, status);
}

inline drogon::HttpResponsePtr api_exception(const drogon::HttpRequest& request, const std::exception& error) {
    using namespace drogon;
    if (const auto* validation = dynamic_cast<const RequestValidationError*>(&error)) {
        return api_error(request, k400BadRequest, "VALIDATION_FAILED", validation->what(), validation->fields);
    }
    if (const auto* sql = dynamic_cast<const orm::SqlError*>(&error)) {
        constexpr int sqlite_constraint = 19;
        if ((sql->errcode() & 0xff) == sqlite_constraint ||
            (sql->extendedErrcode() & 0xff) == sqlite_constraint ||
            sql->sqlState().starts_with("23")) {
            return api_error(request, k409Conflict, "CONFLICT", "Resource conflicts with a database constraint.");
        }
    }
    return api_error(request, k500InternalServerError, "INTERNAL_ERROR", "Internal server error.");
}

}
