#include "app_namespace.hpp"
#include "routes.hpp"
#include "server/json_response.hpp"
#include "server/services.hpp"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {
struct UploadResult {
    std::string url;
};

namespace {

using namespace drogon;

constexpr auto uploadPrefix = "/public/uploads/";

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
                co_return jsonError(k400BadRequest, "Choose one file to upload.");
            case UploadError::tooLarge:
                co_return jsonError(k413RequestEntityTooLarge, "The file exceeds 10 MiB.");
            case UploadError::unsupported:
                co_return jsonError(k415UnsupportedMediaType, "Choose a PDF or a supported image.");
        }
    }

    const auto url = std::string(uploadPrefix) + *saved;
    auto response = jsonResponse(UploadResult{url}, k201Created);
    response->addHeader("Location", url);
    co_return response;
}

Task<HttpResponsePtr> download(std::shared_ptr<Services> services, HttpRequestPtr request,
                               std::string name) {
    co_return co_await services->workers.submit([&] {
        const auto file = services->uploads.find(name);
        if (!file) return jsonError(k404NotFound, "Upload not found.");
        auto response = HttpResponse::newFileResponse(file->first.string(), name, CT_CUSTOM,
                                                      std::string(file->second.mime), request);
        response->addHeader("X-Content-Type-Options", "nosniff");
        return response;
    });
}

}

void registerUploadRoutes(const std::shared_ptr<Services>& services, const std::string& jwt) {
    auto& server = app();
    server.registerHandler("/api/uploads", [services](HttpRequestPtr request) {
        return upload(services, std::move(request));
    }, {Post, jwt});
    server.registerHandler("/public/uploads/{1}", [services](HttpRequestPtr request,
                                                              std::string name) {
        return download(services, std::move(request), std::move(name));
    }, {Get});
}

}
