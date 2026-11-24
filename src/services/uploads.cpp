#include "app_namespace.hpp"
#include "uploads.hpp"
#include "utils/random.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <system_error>

namespace APP_NAMESPACE {
namespace {

constexpr UploadType pdf{"pdf", "application/pdf"};
constexpr UploadType png{"png", "image/png"};
constexpr UploadType jpeg{"jpg", "image/jpeg"};
constexpr UploadType gif{"gif", "image/gif"};
constexpr UploadType webp{"webp", "image/webp"};

std::optional<UploadType> detect(std::string_view bytes) {
    if (bytes.starts_with("%PDF-")) return pdf;
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return png;
    if (bytes.starts_with("\xff\xd8\xff")) return jpeg;
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return gif;
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") {
        return webp;
    }
    return std::nullopt;
}

[[noreturn]] void writeError(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}

class TemporaryUpload {
public:
    explicit TemporaryUpload(const std::filesystem::path& directory)
        : path_((directory / ".pending-XXXXXX").string()) {
        descriptor_ = mkstemp(path_.data());
        if (descriptor_ == -1) writeError("could not create temporary upload");
    }

    TemporaryUpload(const TemporaryUpload&) = delete;
    TemporaryUpload& operator=(const TemporaryUpload&) = delete;

    ~TemporaryUpload() {
        if (descriptor_ != -1) close(descriptor_);
        unlink(path_.c_str());
    }

    void writeAll(std::string_view data) {
        while (!data.empty()) {
            const auto count = write(descriptor_, data.data(), data.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) writeError("could not write upload");
            if (count == 0) throw std::runtime_error("upload write made no progress");
            data.remove_prefix(static_cast<std::size_t>(count));
        }
        const int descriptor = std::exchange(descriptor_, -1);
        if (close(descriptor) != 0) writeError("could not close upload");
    }

    bool publish(const std::filesystem::path& destination) const {
        if (link(path_.c_str(), destination.c_str()) == 0) return true;
        if (errno == EEXIST) return false;
        writeError("could not publish upload");
    }

private:
    std::string path_;
    int descriptor_;
};

}

std::expected<std::string, UploadError> Uploads::save(std::string_view bytes) const {
    if (bytes.empty()) return std::unexpected(UploadError::empty);
    if (bytes.size() > maxUploadBytes) return std::unexpected(UploadError::tooLarge);
    const auto type = detect(bytes);
    if (!type) return std::unexpected(UploadError::unsupported);

    TemporaryUpload temporary(directory_);
    temporary.writeAll(bytes);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto name = randomHex<16>() + "." + std::string(type->extension);
        if (temporary.publish(directory_ / name)) return name;
    }
    throw std::runtime_error("could not allocate a unique upload name");
}

std::optional<std::pair<std::filesystem::path, UploadType>> Uploads::find(
    std::string_view name) const {
    if (name.size() < 34 || name[32] != '.' ||
        name.substr(0, 32).find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        return std::nullopt;
    }

    const auto extension = name.substr(33);
    for (const auto type : {pdf, png, jpeg, gif, webp}) {
        if (extension == type.extension) {
            auto path = directory_ / name;
            if (std::filesystem::is_regular_file(std::filesystem::symlink_status(path))) {
                return std::pair{std::move(path), type};
            }
        }
    }
    return std::nullopt;
}

}
