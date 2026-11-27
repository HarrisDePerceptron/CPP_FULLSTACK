#pragma once

#include "app_namespace.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

inline constexpr std::size_t max_upload_bytes = 10 * 1024 * 1024;
enum class UploadError { empty,
                         too_large,
                         unsupported };

struct UploadType {
    std::string_view extension;
    std::string_view mime;
};

class Uploads {
  public:
    explicit Uploads(std::filesystem::path directory, std::size_t maximum_bytes = max_upload_bytes)
        : directory_(std::move(directory)), maximum_bytes_(maximum_bytes) {}

    std::expected<std::string, UploadError> save(std::string_view bytes) const;
    std::optional<std::pair<std::filesystem::path, UploadType>> find(std::string_view name) const;
    std::size_t maximum_bytes() const { return maximum_bytes_; }

  private:
    std::filesystem::path directory_;
    std::size_t maximum_bytes_;
};

}
