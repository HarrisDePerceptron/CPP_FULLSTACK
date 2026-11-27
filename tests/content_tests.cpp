#include "app_namespace.h"
#include "app/services/content_client.h"

#include <catch2/catch_test_macros.hpp>
#include <trantor/net/EventLoopThread.h>

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
using namespace APP_NAMESPACE;
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

        if (listener_ < 0)
            throw std::runtime_error("could not create fixture socket");
        try {
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
                listen(listener_, 1) != 0 ||
                getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
                throw std::runtime_error("could not bind local HTTP fixture");
            }
            port_ = ntohs(address.sin_port);
            const auto response = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                                                         "Connection: close\r\nContent-Length: " +
                                  std::to_string(body.size()) +
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

    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }

  private:
    void reply(std::string_view response, std::chrono::milliseconds delay) const {
        pollfd incoming{listener_, POLLIN, 0};
        if (poll(&incoming, 1, 3000) <= 0)
            return;
        const int client = accept(listener_, nullptr, nullptr);
        if (client == -1)
            return;

#ifdef SO_NOSIGPIPE
        constexpr int enabled = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
        constexpr int send_flags = 0;
#else
        constexpr int send_flags = MSG_NOSIGNAL;
#endif
        pollfd readable{client, POLLIN, 0};
        std::array<char, 4096> buffer;
        if (poll(&readable, 1, 3000) > 0 && recv(client, buffer.data(), buffer.size(), 0) > 0) {
            std::this_thread::sleep_for(delay);
            while (!response.empty()) {
                const auto sent = send(client, response.data(), response.size(), send_flags);
                if (sent <= 0)
                    break;
                response.remove_prefix(static_cast<std::size_t>(sent));
            }
        }
        close(client);
    }

    int listener_;
    unsigned port_{};
    std::thread thread_;
};

std::expected<Content, ContentError> fetch_local_content(
    const LocalResponse& fixture, std::chrono::milliseconds timeout = 5s) {
    trantor::EventLoopThread thread;
    thread.run();
    const auto run = [&]() -> drogon::Task<std::expected<Content, ContentError>> {
        co_await drogon::switchThreadCoro(thread.getLoop());
        co_return co_await fetch_content(fixture.origin(), "/todo", timeout);
    };
    return drogon::sync_wait(run());
}

TEST_CASE("The content client fetches and validates JSON over HTTP", "[content]") {
    const LocalResponse fixture("200 OK", R"({"id":1,"title":"Test todo","completed":false})");
    const auto content = fetch_local_content(fixture);
    REQUIRE(content.has_value());
    CHECK(content->id == 1);
    CHECK(content->title == "Test todo");
    CHECK_FALSE(content->completed);
}

TEST_CASE("Upstream HTTP and JSON errors, oversized replies, and timeouts are handled", "[content]") {
    SECTION("HTTP error") {
        const LocalResponse fixture("503 Unavailable", "{}");
        CHECK(fetch_local_content(fixture).error() == ContentError::upstream);
    }
    SECTION("Bad JSON or missing fields") {
        const LocalResponse fixture("200 OK", R"({"title":true})");
        CHECK(fetch_local_content(fixture).error() == ContentError::upstream);
    }
    SECTION("Oversized response") {
        const LocalResponse fixture("200 OK", std::string(256 * 1024 + 1, 'x'));
        CHECK(fetch_local_content(fixture).error() == ContentError::upstream);
    }
    SECTION("Timeout") {
        const LocalResponse fixture("200 OK", "{}", 200ms);
        CHECK(fetch_local_content(fixture, 50ms).error() == ContentError::timeout);
    }
}
}
