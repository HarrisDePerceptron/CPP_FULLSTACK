#include "content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
using namespace hello;
using namespace std::chrono_literals;

class LocalResponse {
public:
    LocalResponse(std::string status, std::string body,
                  std::chrono::milliseconds delay = 0ms) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);

        if (listener_ < 0) throw std::runtime_error("could not create fixture socket");
        try {
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
                listen(listener_, 1) != 0 ||
                getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
                throw std::runtime_error("could not bind local HTTP fixture");
            }
            port_ = ntohs(address.sin_port);
            const auto response = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                "Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\n\r\n" + body;
            thread_ = std::thread([this, response, delay] { reply(response, delay); });
        } catch (...) {
            close(listener_);
            throw;
        }
    }

    ~LocalResponse() {
        thread_.join();
        close(listener_);
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/todo"; }

private:
    void reply(std::string_view response, std::chrono::milliseconds delay) const {
        pollfd incoming{listener_, POLLIN, 0};
        if (poll(&incoming, 1, 3000) <= 0) return;
        const int client = accept(listener_, nullptr, nullptr);
        if (client == -1) return;

#ifdef SO_NOSIGPIPE
        constexpr int enabled = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
        constexpr int sendFlags = 0;
#else
        constexpr int sendFlags = MSG_NOSIGNAL;
#endif
        pollfd readable{client, POLLIN, 0};
        std::array<char, 4096> buffer;
        if (poll(&readable, 1, 3000) > 0 && recv(client, buffer.data(), buffer.size(), 0) > 0) {
            std::this_thread::sleep_for(delay);
            while (!response.empty()) {
                const auto sent = send(client, response.data(), response.size(), sendFlags);
                if (sent <= 0) break;
                response.remove_prefix(static_cast<std::size_t>(sent));
            }
        }
        close(client);
    }

    int listener_;
    unsigned port_{};
    std::thread thread_;
};

TEST_CASE("CPR fetches local JSON and simdjson validates the content payload", "[content]") {
    const LocalResponse fixture("200 OK", R"({"id":1,"title":"Test todo","completed":false})");
    const auto content = fetchContent(fixture.url());
    REQUIRE(content.has_value());
    CHECK(content->id == 1);
    CHECK(content->title == "Test todo");
    CHECK_FALSE(content->completed);
}

TEST_CASE("Upstream HTTP and JSON errors, oversized replies, and timeouts are handled", "[content]") {
    SECTION("HTTP error") {
        const LocalResponse fixture("503 Unavailable", "{}");
        CHECK(fetchContent(fixture.url()).error() == ContentError::upstream);
    }
    SECTION("Bad JSON or missing fields") {
        const LocalResponse fixture("200 OK", R"({"title":true})");
        CHECK(fetchContent(fixture.url()).error() == ContentError::upstream);
    }
    SECTION("Oversized response") {
        const LocalResponse fixture("200 OK", std::string(256 * 1024 + 1, 'x'));
        CHECK(fetchContent(fixture.url()).error() == ContentError::upstream);
    }
    SECTION("Timeout") {
        const LocalResponse fixture("200 OK", "{}", 200ms);
        CHECK(fetchContent(fixture.url(), 50ms).error() == ContentError::timeout);
    }
}
}
