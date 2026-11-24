#include "content.hpp"

#include <cpr/cpr.h>
#include <simdjson.h>

namespace hello {

std::expected<Content, ContentError> fetchContent(std::string_view url,
                                                 std::chrono::milliseconds timeout) {
    constexpr std::size_t bodyLimit = 256 * 1024;
    std::string body;
    const auto response = cpr::Get(
        cpr::Url{std::string(url)}, cpr::Timeout{timeout},
        cpr::Header{{"Accept", "application/json"}}, cpr::Redirect{false},
        cpr::WriteCallback{[&body](const std::string_view& chunk, intptr_t) {
            if (chunk.size() > bodyLimit - body.size()) return false;
            body.append(chunk);
            return true;
        }});

    if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return std::unexpected(ContentError::timeout);
    }
    if (response.error || response.status_code != 200) {
        return std::unexpected(ContentError::upstream);
    }

    simdjson::dom::parser parser;
    const simdjson::padded_string padded(body);
    simdjson::dom::element json;
    Content content{};
    std::string_view title;
    if (parser.parse(padded).get(json) || json["id"].get(content.id) ||
        json["title"].get(title) || json["completed"].get(content.completed)) {
        return std::unexpected(ContentError::upstream);
    }
    content.title = title;
    return content;
}

}
