#pragma once

#include "app_namespace.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

inline constexpr std::size_t maxUploadBytes = 10 * 1024 * 1024;
enum class UploadError { empty, tooLarge, unsupported };

struct UploadType {
    std::string_view extension;
    std::string_view mime;
};

class Uploads {
public:
    explicit Uploads(std::filesystem::path directory) : directory_(std::move(directory)) {}

    std::expected<std::string, UploadError> save(std::string_view bytes) const;
    std::optional<std::pair<std::filesystem::path, UploadType>> find(std::string_view name) const;

private:
    std::filesystem::path directory_;
};

}
