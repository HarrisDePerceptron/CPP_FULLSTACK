#include "app/server/dto/upload_dtos.h"
#include "app/server/routes.h"
#include "framework/auth/auth.h"
#include "framework/http/json_response.h"
#include "framework/http/server_runtime.h"
#include "framework/storage/uploads.h"

#include <drogon/drogon.h>

namespace APP_NAMESPACE {

namespace {

using namespace drogon;

Task<HttpResponsePtr> upload(const ServerRuntime& runtime,
                             HttpRequestPtr request) {
    const auto saved = co_await runtime.run_blocking(
        [&] -> std::expected<std::string, UploadError> {
            MultiPartParser parser;
            if (parser.parse(request) != 0 || parser.getFiles().size() != 1 ||
                parser.getFiles().front().getItemName() != "file") {
                return std::unexpected(UploadError::empty);
            }
            return runtime.uploads().save(
                parser.getFiles().front().fileContent());
        });

    if (!saved) {
        switch (saved.error()) {
        case UploadError::empty:
            co_return api_error(*request, k400BadRequest, "VALIDATION_FAILED",
                                "Choose one file to upload.");
        case UploadError::too_large: {
            const auto limit = runtime.uploads().maximum_bytes();
            auto message = limit == max_upload_bytes
                               ? std::string("The file exceeds 10 MiB.")
                               : "The file exceeds the " +
                                     std::to_string(limit) + " byte limit.";
            co_return api_error(*request, k413RequestEntityTooLarge,
                                "PAYLOAD_TOO_LARGE", std::move(message));
        }
        case UploadError::unsupported:
            co_return api_error(*request, k415UnsupportedMediaType,
                                "UNSUPPORTED_MEDIA_TYPE",
                                "Choose a PDF or a supported image.");
        }
    }

    const auto url = runtime.config().uploads_url_prefix + *saved;
    auto response = api_ok(*request, UploadResult{url}, k201Created);
    response->addHeader("Location", url);
    co_return response;
}

Task<HttpResponsePtr> download(const ServerRuntime& runtime,
                               HttpRequestPtr request, std::string name) {
    co_return co_await runtime.run_blocking([&] {
        const auto file = runtime.uploads().find(name);
        if (!file)
            return api_error(*request, k404NotFound, "NOT_FOUND",
                             "Upload not found.");
        auto response = HttpResponse::newFileResponse(
            file->first.string(), name, CT_CUSTOM,
            std::string(file->second.mime), request);
        response->addHeader("X-Content-Type-Options", "nosniff");
        return response;
    });
}

}

void register_upload_routes(const ServerRuntime& runtime) {
    auto routes = runtime.routes().with(std::cref(runtime));
    const auto& prefix = runtime.config().uploads_url_prefix;
    routes.post("/api/uploads", upload, Auth<>());
    routes.get(prefix + "{1}", download);
}

}
