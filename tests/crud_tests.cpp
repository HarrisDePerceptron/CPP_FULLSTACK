#include "app/server/dto/account_dtos.h"
#include "app/server/routes.h"
#include "app/server/services.h"
#include "framework/auth/auth.h"
#include "framework/db/base_dao.h"
#include "framework/crud/crud_router.h"
#include "framework/http/server_hooks.h"
#include "framework/support/workers.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <future>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sodium.h>
#include <unistd.h>

using namespace APP_NAMESPACE;

namespace crud_test {

struct WidgetFields {
    static constexpr std::string_view table_name = "crud_widgets";
    Unique<std::string> name;
    std::string group;
    std::optional<std::string> note;
    std::optional<std::string> private_value;
};

constexpr auto fixture_signing_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

using WidgetRecord = DaoRecord<WidgetFields>;

class WidgetDao final : public BaseDao<WidgetDao, WidgetRecord> {
  public:
    static drogon::Task<WidgetDao> init(DatabaseBackendPtr database) {
        return finish_initialization(WidgetDao(std::move(database)));
    }

  private:
    explicit WidgetDao(DatabaseBackendPtr database)
        : BaseDao(std::move(database)) {}
};

struct Widgets {
    struct Response {
        std::int64_t id;
        std::string name;
        std::string group;
        std::optional<std::string> note;
    };
    struct Create {
        std::string name, group;
        std::optional<std::string> note;
    };
    struct Patch {
        std::optional<std::string> name;
        std::optional<std::optional<std::string>> note;
    };

    static Response to_response(const WidgetRecord& row) {
        return {*row.id, row.name.value, row.group, row.note};
    }

    static std::string required_text(const Json::Value& value) {
        if (!value.isString() || value.asString().empty())
            throw CrudValidationError("Expected nonempty text.");
        return value.asString();
    }

    static std::optional<std::string> nullable_text(const Json::Value& value) {
        if (value.isNull())
            return std::nullopt;
        return required_text(value);
    }

    static Create parse_create(const Json::Value& body) {
        for (const auto& name : body.getMemberNames()) {
            if (name != "name" && name != "group" && name != "note") {
                throw CrudValidationError("Unknown create field.");
            }
        }
        return {required_text(body["name"]), required_text(body["group"]),
                nullable_text(body["note"])};
    }

    static Patch parse_patch(const Json::Value& body) {
        Patch patch;
        for (const auto& name : body.getMemberNames()) {
            if (name == "name")
                patch.name = required_text(body[name]);
            else if (name == "note")
                patch.note.emplace(nullable_text(body[name]));
            else
                throw CrudValidationError("Unknown patch field.");
        }
        return patch;
    }

    static WidgetRecord make_record(Create input) {
        return {{std::move(input.name), std::move(input.group),
                 std::move(input.note), "private"}};
    }

    static void apply_patch(WidgetRecord& row, Patch input) {
        if (input.name)
            row.name.value = std::move(*input.name);
        if (input.note)
            row.note = std::move(*input.note);
    }

    static void add_filter(WidgetDao::EqualityFilters& filters,
                           std::string_view name, std::string value) {
        if (name == "name")
            filters.equal<&WidgetFields::name>(std::move(value));
        else if (name == "group")
            filters.equal<&WidgetFields::group>(std::move(value));
        else
            throw CrudValidationError("Unknown filter.");
    }
};

class WidgetService final : public CrudService<WidgetDao, Widgets> {
  public:
    using CrudService::CrudService;

    drogon::Task<std::optional<Response>> get(Id id) const override {
        auto item = co_await CrudService::get(id);
        if (item)
            item->group += "-service";
        co_return item;
    }
};

class WidgetRouter final : public CrudRouter<WidgetService> {
  public:
    using CrudRouter::CrudRouter;

  protected:
    drogon::Task<std::optional<Response>> get(const Request& request,
                                              Id id) const override {
        auto item = co_await CrudRouter::get(request, id);
        if (item)
            item->group += "-router";
        co_return item;
    }
};

unsigned short free_port() {
    const auto socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        throw std::runtime_error("Could not allocate fixture socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const auto status =
        bind(socket_fd, reinterpret_cast<sockaddr*>(&address), size) == 0
            ? getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address),
                          &size)
            : -1;
    close(socket_fd);
    if (status != 0)
        throw std::runtime_error("Could not reserve fixture port");
    return ntohs(address.sin_port);
}

Json::Value check_reply(const drogon::HttpResponsePtr& response, int status,
                        ApiPagination pagination = {}) {
    REQUIRE(response->statusCode() == status);
    const auto parsed = response->getJsonObject();
    REQUIRE(parsed);
    const auto& json = *parsed;
    REQUIRE(json.getMemberNames().size() == 3);
    CHECK(json["success"].asBool() == (status < 400));
    const auto& meta = json["meta"];
    const auto timestamp = meta["timestamp"].asString();
    REQUIRE(timestamp.size() == 20);
    CHECK(timestamp.ends_with("Z"));
    CHECK(meta["request_id"].asString().starts_with("req_"));
    CHECK(response->getHeader("X-Request-ID") == meta["request_id"].asString());
    if (status >= 400) {
        CHECK_FALSE(json.isMember("data"));
        CHECK_FALSE(meta.isMember("pagination"));
        CHECK_FALSE(json["error"]["code"].asString().empty());
        CHECK_FALSE(json["error"]["message"].asString().empty());
    } else {
        CHECK_FALSE(json.isMember("error"));
        REQUIRE(json["data"].isArray());
        const auto& page = meta["pagination"];
        REQUIRE(page.getMemberNames().size() == 3);
        CHECK(page["page"].asInt64() == pagination.page);
        CHECK(page["per_page"].asInt64() == pagination.per_page);
        CHECK(page["has_more"].asBool() == pagination.has_more);
    }
    return json["data"];
}

class CrudServer {
  public:
    CrudServer() : port_(free_port()) {
        auto ready = std::make_shared<std::promise<void>>();
        auto startup = ready->get_future();
        server_ = std::async(std::launch::async, [this, ready] {
            try {
                auto& server = drogon::app();
                server.addListener("127.0.0.1", port_).setThreadNum(1);
                register_exception_handler(server, Logger(LogLevel::error));
                server.registerHandler(
                    "/exceptions/busy",
                    [](drogon::HttpRequestPtr)
                        -> drogon::Task<drogon::HttpResponsePtr> {
                        throw WorkersBusy{};
                        co_return nullptr;
                    },
                    {drogon::Get});
                server.registerHandler(
                    "/exceptions/unexpected",
                    [](drogon::HttpRequestPtr)
                        -> drogon::Task<drogon::HttpResponsePtr> {
                        throw std::runtime_error("private fixture failure");
                        co_return nullptr;
                    },
                    {drogon::Get});
                for (const std::string prefix :
                     {"/success", "/errors", "/edges"}) {
                    auto database =
                        drogon::sync_wait(open_sqlite_database(":memory:"));
                    auto service = std::make_shared<WidgetService>(
                        drogon::sync_wait(WidgetDao::init(database)));
                    WidgetRouter::mount(
                        server, prefix + "/widgets",
                        std::make_shared<WidgetRouter>(service));
                    WidgetRouter::mount(
                        server, prefix + "/readonly",
                        std::make_shared<WidgetRouter>(
                            service, CrudMethods{.create = false,
                                                 .patch = false,
                                                 .remove = false}));
                }
                this->mount_social(server);
                const detail::AuthVerifierScope auth_verifier(
                    {social_services_, &social_services_->tokens});
                server.registerBeginningAdvice([ready] { ready->set_value(); });
                server.run();
            } catch (...) {
                ready->set_exception(std::current_exception());
            }
        });
        startup.get();
    }

    ~CrudServer() {
        drogon::app().quit();
        server_.wait();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    const std::string& social_token() const { return social_token_; }

    drogon::Task<bool> set_roles(std::string username,
                                 std::vector<Role> roles) const {
        const auto id =
            co_await social_services_->users.find_id(std::move(username));
        if (!id)
            co_return false;
        co_return co_await social_services_->users.dao().set_roles(
            *id, std::move(roles));
    }

  private:
    void mount_social(drogon::HttpAppFramework& server) {
        if (sodium_init() < 0)
            throw std::runtime_error("Could not initialize libsodium");
        auto database = drogon::sync_wait(open_sqlite_database(":memory:"));
        auto users = drogon::sync_wait(UserDao::init(database));
        auto posts = drogon::sync_wait(SocialPostDao::init(database));
        auto comments = drogon::sync_wait(SocialCommentDao::init(database));
        auto likes = drogon::sync_wait(SocialLikeDao::init(database));
        constexpr auto username = "social-fixture";
        drogon::sync_wait(users.create({{username, "unused"}}));
        social_services_ = std::make_shared<Services>(
            ResolvedServerConfig{}, Config{.jwt_lifetime_seconds = 1800},
            Logger(LogLevel::error), fixture_signing_key,
            UserService(std::move(users)),
            SocialService(std::move(posts), std::move(comments),
                          std::move(likes)));
        const auto& services = social_services_;
        social_token_ = services->tokens.issue(username);
        register_auth_routes(server, services);
        register_social_routes(server, services, "/public/");
        server.registerHandler("/fixture/auth/public",
                               [](drogon::HttpRequestPtr request)
                                   -> drogon::Task<drogon::HttpResponsePtr> {
                                   co_return api_ok(*request);
                               },
                               {drogon::Get});
        const auto inspect_username = [](drogon::HttpRequestPtr request)
            -> drogon::Task<drogon::HttpResponsePtr> {
            co_return api_ok(*request,
                             Identity{get_current_username(*request)});
        };
        server.registerHandler("/fixture/auth/any", inspect_username,
                               {drogon::Get, Auth<>()});
        server.registerHandler("/fixture/auth/admin", inspect_username,
                               {drogon::Get, Auth<Role::admin>()});
        server.registerHandler(
            "/fixture/auth/either", inspect_username,
            {drogon::Get, Auth<Role::member, Role::admin>()});
    }

    unsigned short port_;
    std::string social_token_;
    std::shared_ptr<Services> social_services_;
    std::future<void> server_;
};

class CrudClient {
  public:
    explicit CrudClient(std::string prefix, std::string bearer = {},
                        std::string session = {})
        : prefix_(std::move(prefix)), bearer_(std::move(bearer)),
          session_(std::move(session)),
          client_(drogon::HttpClient::newHttpClient(server().base_url())) {}

    static CrudClient social() {
        return CrudClient("", server().social_token());
    }

    static bool set_roles(std::string username, std::vector<Role> roles) {
        return drogon::sync_wait(
            server().set_roles(std::move(username), std::move(roles)));
    }

    drogon::HttpResponsePtr get(const std::string& path) const {
        return send(drogon::Get, path);
    }

    drogon::HttpResponsePtr post(const std::string& path,
                                 const std::string& body,
                                 double timeout_seconds = 2) const {
        return send(drogon::Post, path, body, timeout_seconds);
    }

    drogon::HttpResponsePtr patch(const std::string& path,
                                  const std::string& body) const {
        return send(drogon::Patch, path, body);
    }

    drogon::HttpResponsePtr remove(const std::string& path) const {
        return send(drogon::Delete, path);
    }

  private:
    drogon::HttpResponsePtr send(drogon::HttpMethod method,
                                 const std::string& path,
                                 const std::string& body = {},
                                 double timeout_seconds = 2) const {
        CAPTURE(path);
        const auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        request->setPath(prefix_ + path);
        request->setPathEncode(false);
        if (!bearer_.empty())
            request->addHeader("Authorization", "Bearer " + bearer_);
        if (!session_.empty())
            request->addCookie(session_cookie_name, session_);
        if (method == drogon::Post || method == drogon::Patch) {
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(body);
        }
        auto [result, response] =
            client_->sendRequest(request, timeout_seconds);
        REQUIRE(result == drogon::ReqResult::Ok);
        return response;
    }

    static const CrudServer& server() {
        static const CrudServer fixture;
        return fixture;
    }

    std::string prefix_;
    std::string bearer_;
    std::string session_;
    drogon::HttpClientPtr client_;
};

std::string create_widget(const CrudClient& client) {
    const auto created = check_reply(
        client.post("/widgets",
                    R"({"name":"alpha","group":"one","note":"keep"})"),
        201);
    REQUIRE(created.size() == 1);
    CHECK_FALSE(created[0].isMember("private_value"));
    return "/widgets/" + std::to_string(created[0]["id"].asInt64());
}

}

TEST_CASE("The exception hook returns a 503 for a busy worker pool", "[http]") {
    const crud_test::CrudClient client("/exceptions");
    const auto response = client.get("/busy");
    crud_test::check_reply(response, 503);
    const auto json = response->getJsonObject();
    CHECK((*json)["error"]["code"].asString() == "SERVICE_UNAVAILABLE");
}

TEST_CASE("The exception hook returns a generic 500 for unexpected errors",
          "[http]") {
    const crud_test::CrudClient client("/exceptions");
    const auto response = client.get("/unexpected");
    crud_test::check_reply(response, 500);
    const auto json = response->getJsonObject();
    CHECK((*json)["error"]["code"].asString() == "INTERNAL_ERROR");
    CHECK((*json)["error"]["message"].asString() == "Internal server error.");
}

TEST_CASE("The current username helper requires authenticated request context",
          "[http][jwt]") {
    const auto request = drogon::HttpRequest::newHttpRequest();
    CHECK_THROWS_AS(get_current_username(*request), std::logic_error);
}

TEST_CASE("A protected route fails closed without a configured verifier",
          "[http][jwt]") {
    using namespace crud_test;
    const auto client = CrudClient::social();
    {
        const detail::AuthVerifierScope without_verifier(nullptr);
        check_reply(CrudClient("").get("/fixture/auth/public"), 200);
        check_reply(client.get("/fixture/auth/any"), 500);
    }
    check_reply(client.get("/fixture/auth/any"), 200);
}

TEST_CASE("Auth rejects missing credentials and checks roles before invoking "
          "the handler",
          "[http][jwt]") {
    using namespace crud_test;
    const CrudClient public_client("");
    const auto missing = public_client.get("/fixture/auth/any");
    check_reply(missing, 401);
    CHECK(missing->getHeader("WWW-Authenticate") == "Bearer");
    check_reply(CrudClient("", "bad-token").get("/fixture/auth/admin"), 401);

    const auto account = CrudClient::social();
    const auto identity = check_reply(account.get("/fixture/auth/any"), 200);
    REQUIRE(identity.size() == 1);
    CHECK(identity[0]["username"].asString() == "social-fixture");
    const auto forbidden = account.get("/fixture/auth/either");
    check_reply(forbidden, 403);
    CHECK((*forbidden->getJsonObject())["error"]["code"].asString() ==
          "FORBIDDEN");
}

TEST_CASE("Login signs stored roles and role changes appear in a fresh token",
          "[http][jwt]") {
    using namespace crud_test;
    const CrudClient public_client("");
    constexpr auto credentials =
        R"({"username":"role-fixture","password":"example-password"})";
    constexpr double password_timeout_seconds = 10;
    check_reply(
        public_client.post(
            "/api/signup",
            R"({"username":"role-fixture","password":"example-password","roles":["admin"]})"),
        400);
    check_reply(public_client.post("/api/signup", credentials,
                                   password_timeout_seconds),
                201);
    const auto login = [&] {
        const auto reply =
            check_reply(public_client.post("/api/login", credentials,
                                           password_timeout_seconds),
                        200);
        REQUIRE(reply.size() == 1);
        return reply[0]["access_token"].asString();
    };

    const auto member_token = login();
    const CrudClient member("", member_token);
    const Tokens tokens(fixture_signing_key, std::chrono::seconds(1800));
    const auto member_claims = tokens.verify(member_token);
    REQUIRE(member_claims);
    CHECK(member_claims->roles<Role>() == std::vector{Role::member});
    const auto member_identity =
        check_reply(member.get("/fixture/auth/either"), 200);
    REQUIRE(member_identity.size() == 1);
    CHECK(member_identity[0]["username"].asString() == "role-fixture");
    check_reply(member.get("/fixture/auth/admin"), 403);

    REQUIRE(CrudClient::set_roles("role-fixture",
                                  {Role::admin, Role::member, Role::admin}));
    const auto admin_token = login();
    const CrudClient admin("", "", admin_token);
    const auto admin_claims = tokens.verify(admin_token);
    REQUIRE(admin_claims);
    CHECK(admin_claims->roles<Role>() ==
          std::vector{Role::admin, Role::member});
    const auto admin_identity =
        check_reply(admin.get("/fixture/auth/admin"), 200);
    REQUIRE(admin_identity.size() == 1);
    CHECK(admin_identity[0]["username"].asString() == "role-fixture");
    const auto profile = check_reply(admin.get("/api/me"), 200);
    REQUIRE(profile.size() == 1);
    CHECK(profile[0]["username"].asString() == "role-fixture");
    check_reply(admin.get("/fixture/auth/either"), 200);
    check_reply(
        CrudClient("", "bad-token", admin_token).get("/fixture/auth/admin"),
        401);

    REQUIRE(CrudClient::set_roles("role-fixture", {Role::member}));
    check_reply(admin.get("/fixture/auth/admin"), 200);
    check_reply(CrudClient("", login()).get("/fixture/auth/admin"), 403);
}

TEST_CASE("Social routes trim submitted text and enforce Unicode limits",
          "[http][social]") {
    using namespace crud_test;
    const auto client = CrudClient::social();
    constexpr auto emoji = "\xf0\x9f\x98\x80";
    std::string text;
    for (int index = 0; index < 280; ++index)
        text += emoji;

    const auto post = check_reply(
        client.post("/api/posts", "{\"text\":\"  " + text + "  \"}"), 201);
    REQUIRE(post.size() == 1);
    CHECK(post[0]["text"].asString() == text);
    const auto comments =
        "/api/posts/" + std::to_string(post[0]["id"].asInt64()) + "/comments";
    check_reply(
        client.post("/api/posts", "{\"text\":\"" + text + emoji + "\"}"), 400);

    for (int index = 280; index < 500; ++index)
        text += emoji;
    const auto reply = check_reply(
        client.post(comments, "{\"text\":\"  " + text + "  \"}"), 201);
    REQUIRE(reply.size() == 1);
    CHECK(reply[0]["text"].asString() == text);
    check_reply(client.post(comments, "{\"text\":\"" + text + emoji + "\"}"),
                400);
    check_reply(client.post(comments, R"({"text":"   "})"), 400);
}

TEST_CASE("Post creation rejects blank text, NULs, and malformed UTF-8",
          "[http][social]") {
    using namespace crud_test;
    const auto client = CrudClient::social();
    for (const auto body : {R"({"text":" \n\t "})", R"({"text":"a\u0000b"})",
                            "{\"text\":\"\xed\xa0\x80\"}"}) {
        const auto response = client.post("/api/posts", body);
        check_reply(response, 400);
        CHECK((*response->getJsonObject())["error"]["code"].asString() ==
              "VALIDATION_FAILED");
    }
}

TEST_CASE("Social search trims text and accepts empty searches",
          "[http][social]") {
    using namespace crud_test;
    const auto client = CrudClient::social();
    const auto blank = check_reply(client.get("/api/search?q=%20%09"), 200);
    REQUIRE(blank.size() == 1);
    CHECK(blank[0]["posts"].empty());
    CHECK(blank[0]["users"].empty());
    const auto matches =
        check_reply(client.get("/api/search?q=%20social-fixture%20"), 200);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0]["users"].size() == 1);
    CHECK(matches[0]["users"][0]["username"].asString() == "social-fixture");

    std::string query = "/api/search?q=";
    for (int index = 0; index < 128; ++index)
        query += "%F0%9F%98%80";
    check_reply(client.get(query), 200);
    check_reply(client.get(query + "%F0%9F%98%80"), 400);
    check_reply(client.get("/api/search?q=%C0%AF"), 400);
    check_reply(client.get("/api/search?q=a%00b"), 400);
}

TEST_CASE("CRUD routes create, read, filter, update and delete resources",
          "[crud]") {
    using namespace crud_test;
    const CrudClient client("/success");
    const auto path = create_widget(client);
    check_reply(client.post("/widgets", R"({"name":"beta","group":"one"})"),
                201);

    const auto item = check_reply(client.get(path), 200);
    REQUIRE(item.size() == 1);
    CHECK(item[0]["group"].asString() == "one-service-router");
    CHECK_FALSE(item[0].isMember("private_value"));
    CHECK(check_reply(client.get("/widgets?name=alpha&group=one"), 200,
                      {1, 20, false})
              .size() == 1);
    CHECK(check_reply(client.get("/widgets?group=one&per_page=1&page=2"), 200,
                      {2, 1, false})
              .size() == 1);
    CHECK(check_reply(client.get("/widgets?group=one&per_page=1"), 200,
                      {1, 1, true})
              .size() == 1);
    CHECK(check_reply(client.get("/widgets?group=one&per_page=1&page=3"), 200,
                      {3, 1, false})
              .empty());

    const auto updated =
        check_reply(client.patch(path, R"({"name":"gamma"})"), 200);
    REQUIRE(updated.size() == 1);
    CHECK(updated[0]["name"].asString() == "gamma");
    CHECK(updated[0]["note"].asString() == "keep");
    CHECK(check_reply(client.remove(path), 200).empty());
    check_reply(client.get(path), 404);
}

TEST_CASE(
    "CRUD routes reject invalid requests and roll back conflicting patches",
    "[crud]") {
    using namespace crud_test;
    const CrudClient client("/errors");
    const auto path = create_widget(client);
    check_reply(client.post("/widgets", R"({"name":"beta","group":"one"})"),
                201);

    check_reply(client.post("/widgets", R"({"name":"alpha","group":"one"})"),
                409);
    check_reply(client.get("/widgets?per_page=101"), 400);
    check_reply(client.get("/widgets?private_value=private"), 400);
    check_reply(client.get("/widgets/not-an-id"), 400);
    check_reply(client.patch(path, R"({"name":null})"), 400);
    check_reply(client.patch(path, R"({"private_value":"leak"})"), 400);
    check_reply(client.patch(path, "{"), 400);
    check_reply(client.patch(path, R"({"name":"beta","note":null})"), 409);

    const auto item = check_reply(client.get(path), 200);
    REQUIRE(item.size() == 1);
    CHECK(item[0]["name"].asString() == "alpha");
    CHECK(item[0]["note"].asString() == "keep");
}

TEST_CASE(
    "CRUD routes handle empty results, explicit null and disabled methods",
    "[crud]") {
    using namespace crud_test;
    const CrudClient client("/edges");
    const auto path = create_widget(client);

    CHECK(check_reply(client.get("/widgets?name=alpha&group=two"), 200,
                      {1, 20, false})
              .empty());
    const auto cleared =
        check_reply(client.patch(path, R"({"note":null})"), 200);
    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0]["note"].isNull());

    const auto disabled_post = client.post("/readonly", "{}");
    check_reply(disabled_post, 405);
    CHECK(disabled_post->getHeader("allow") == "GET");

    const auto disabled_patch =
        client.patch("/readonly" + path.substr(path.rfind('/')), "{}");
    check_reply(disabled_patch, 405);
    CHECK(disabled_patch->getHeader("allow") == "GET");

    check_reply(client.remove(path), 200);
    check_reply(client.patch(path, "{}"), 404);
}
