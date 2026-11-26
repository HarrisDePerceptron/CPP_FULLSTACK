#include "app/models/upload_models.h"
#include "app/server/routes.h"
#include "app/server/services.h"
#include "framework/http/json_response.h"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {

namespace {

using namespace drogon;

constexpr auto upload_prefix = "/public/uploads/";

Task<HttpResponsePtr> upload(std::shared_ptr<Services> services, HttpRequestPtr request) {
    const auto saved = co_await services->workers.submit([&]
        -> std::expected<std::string, UploadError> {
        MultiPartParser parser;
        if (parser.parse(request) != 0 || parser.getFiles().size() != 1 ||
            parser.getFiles().front().getItemName() != "file") {
            return std::unexpected(UploadError::empty);
        }
        return services->uploads.save(parser.getFiles().front().fileContent());
    });

    if (!saved) {
        switch (saved.error()) {
            case UploadError::empty:
                co_return api_error(*request, k400BadRequest, "VALIDATION_FAILED", "Choose one file to upload.");
            case UploadError::too_large:
                co_return api_error(*request, k413RequestEntityTooLarge, "PAYLOAD_TOO_LARGE", "The file exceeds 10 MiB.");
            case UploadError::unsupported:
                co_return api_error(*request, k415UnsupportedMediaType, "UNSUPPORTED_MEDIA_TYPE", "Choose a PDF or a supported image.");
        }
    }

    const auto url = std::string(upload_prefix) + *saved;
    auto response = api_ok(*request, UploadResult{url}, k201Created);
    response->addHeader("Location", url);
    co_return response;
}

Task<HttpResponsePtr> download(std::shared_ptr<Services> services, HttpRequestPtr request,
                               std::string name) {
    co_return co_await services->workers.submit([&] {
        const auto file = services->uploads.find(name);
        if (!file) return api_error(*request, k404NotFound, "NOT_FOUND", "Upload not found.");
        auto response = HttpResponse::newFileResponse(file->first.string(), name, CT_CUSTOM,
                                                      std::string(file->second.mime), request);
        response->addHeader("X-Content-Type-Options", "nosniff");
        return response;
    });
}

}

void register_upload_routes(drogon::HttpAppFramework& server, const std::shared_ptr<Services>& services,
                            const std::string& jwt) {
    server.registerHandler("/api/uploads", [services](HttpRequestPtr request) {
        return upload(services, std::move(request));
    }, {Post, jwt});
    server.registerHandler("/public/uploads/{1}", [services](HttpRequestPtr request,
                                                              std::string name) {
        return download(services, std::move(request), std::move(name));
    }, {Get});
}

}
