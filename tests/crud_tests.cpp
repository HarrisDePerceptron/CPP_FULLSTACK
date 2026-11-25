#include "db/base_dao.hpp"
#include "server/routes/crud_router.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cpr/cpr.h>
#include <drogon/drogon.h>
#include <future>
#include <sys/socket.h>
#include <netinet/in.h>
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

using WidgetRecord = DaoRecord<WidgetFields>;

class WidgetDao final : public BaseDao<WidgetDao, WidgetRecord> {
public:
    static drogon::Task<WidgetDao> init(DatabaseBackendPtr database) {
        return finishInitialization(WidgetDao(std::move(database)));
    }
private:
    explicit WidgetDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

struct Widgets {
    struct Response {
        std::int64_t id;
        std::string name;
        std::string group;
        std::optional<std::string> note;
    };
    struct Create { std::string name, group; std::optional<std::string> note; };
    struct Patch {
        std::optional<std::string> name;
        std::optional<std::optional<std::string>> note;
    };

    static Response toResponse(const WidgetRecord& row) {
        return {*row.id, row.name.value, row.group, row.note};
    }

    static std::string requiredText(const Json::Value& value) {
        if (!value.isString() || value.asString().empty()) throw CrudValidationError("Expected nonempty text.");
        return value.asString();
    }

    static std::optional<std::string> nullableText(const Json::Value& value) {
        if (value.isNull()) return std::nullopt;
        return requiredText(value);
    }

    static Create parseCreate(const Json::Value& body) {
        for (const auto& name : body.getMemberNames()) {
            if (name != "name" && name != "group" && name != "note") {
                throw CrudValidationError("Unknown create field.");
            }
        }
        return {requiredText(body["name"]), requiredText(body["group"]), nullableText(body["note"])};
    }

    static Patch parsePatch(const Json::Value& body) {
        Patch patch;
        for (const auto& name : body.getMemberNames()) {
            if (name == "name") patch.name = requiredText(body[name]);
            else if (name == "note") patch.note.emplace(nullableText(body[name]));
            else throw CrudValidationError("Unknown patch field.");
        }
        return patch;
    }

    static WidgetRecord makeRecord(Create input) {
        return {{std::move(input.name), std::move(input.group), std::move(input.note), "private"}};
    }

    static void applyPatch(WidgetRecord& row, Patch input) {
        if (input.name) row.name.value = std::move(*input.name);
        if (input.note) row.note = std::move(*input.note);
    }

    static void addFilter(WidgetDao::EqualityFilters& filters, std::string_view name, std::string value) {
        if (name == "name") filters.equal<&WidgetFields::name>(std::move(value));
        else if (name == "group") filters.equal<&WidgetFields::group>(std::move(value));
        else throw CrudValidationError("Unknown filter.");
    }
};

class WidgetService final : public CrudService<WidgetDao, Widgets> {
public:
    using CrudService::CrudService;

    drogon::Task<std::optional<Response>> get(Id id) const override {
        auto item = co_await CrudService::get(id);
        if (item) item->group += "-service";
        co_return item;
    }
};

class WidgetRouter final : public CrudRouter<WidgetService> {
public:
    using CrudRouter::CrudRouter;

protected:
    drogon::Task<std::optional<Response>> get(const Request& request, Id id) const override {
        auto item = co_await CrudRouter::get(request, id);
        if (item) item->group += "-router";
        co_return item;
    }
};

unsigned short freePort() {
    const auto socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) throw std::runtime_error("Could not allocate fixture socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const auto status = bind(socketFd, reinterpret_cast<sockaddr*>(&address), size) == 0
        ? getsockname(socketFd, reinterpret_cast<sockaddr*>(&address), &size) : -1;
    close(socketFd);
    if (status != 0) throw std::runtime_error("Could not reserve fixture port");
    return ntohs(address.sin_port);
}

Json::Value checkReply(const cpr::Response& response, int status) {
    REQUIRE(response.status_code == status);
    Json::Value json;
    Json::Reader reader;
    REQUIRE(reader.parse(response.text, json));
    REQUIRE(json.getMemberNames().size() == 3);
    REQUIRE(json["result"].isArray());
    REQUIRE(json["error"].isString());
    REQUIRE(json["message"].isString());
    if (status >= 400) {
        CHECK(json["result"].empty());
        CHECK_FALSE(json["error"].asString().empty());
    } else {
        CHECK(json["error"].asString().empty());
    }
    return json["result"];
}

class CrudServer {
public:
    CrudServer() : port_(freePort()) {
        auto ready = std::make_shared<std::promise<void>>();
        auto startup = ready->get_future();
        server_ = std::async(std::launch::async, [this, ready] {
            try {
                auto& server = drogon::app();
                server.addListener("127.0.0.1", port_).setThreadNum(1);
                for (const std::string prefix : {"/success", "/errors", "/edges"}) {
                    auto database = drogon::sync_wait(openSqliteDatabase(":memory:"));
                    auto service = std::make_shared<WidgetService>(drogon::sync_wait(WidgetDao::init(database)));
                    WidgetRouter::mount(server, prefix + "/widgets", std::make_shared<WidgetRouter>(service));
                    WidgetRouter::mount(server, prefix + "/readonly", std::make_shared<WidgetRouter>(
                        service, CrudMethods{.create = false, .patch = false, .remove = false}));
                }
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

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    unsigned short port_;
    std::future<void> server_;
};

class CrudClient {
public:
    explicit CrudClient(const std::string& prefix) : base_(server().baseUrl() + prefix) {}

    cpr::Response get(const std::string& path) const {
        return cpr::Get(cpr::Url{base_ + path}, timeout_);
    }

    cpr::Response post(const std::string& path, const std::string& body) const {
        return cpr::Post(cpr::Url{base_ + path}, jsonHeader_, cpr::Body{body}, timeout_);
    }

    cpr::Response patch(const std::string& path, const std::string& body) const {
        return cpr::Patch(cpr::Url{base_ + path}, jsonHeader_, cpr::Body{body}, timeout_);
    }

    cpr::Response remove(const std::string& path) const {
        return cpr::Delete(cpr::Url{base_ + path}, timeout_);
    }

private:
    static const CrudServer& server() {
        static const CrudServer fixture;
        return fixture;
    }

    std::string base_;
    const cpr::Timeout timeout_{2000};
    const cpr::Header jsonHeader_{{"Content-Type", "application/json"}};
};

std::string createWidget(const CrudClient& client) {
    const auto created = checkReply(client.post("/widgets",
        R"({"name":"alpha","group":"one","note":"keep"})"), 201);
    REQUIRE(created.size() == 1);
    CHECK_FALSE(created[0].isMember("private_value"));
    return "/widgets/" + std::to_string(created[0]["id"].asInt64());
}

}

TEST_CASE("CRUD routes create, read, filter, update and delete resources", "[crud]") {
    using namespace crud_test;
    const CrudClient client("/success");
    const auto path = createWidget(client);
    checkReply(client.post("/widgets", R"({"name":"beta","group":"one"})"), 201);

    const auto item = checkReply(client.get(path), 200);
    REQUIRE(item.size() == 1);
    CHECK(item[0]["group"].asString() == "one-service-router");
    CHECK_FALSE(item[0].isMember("private_value"));
    CHECK(checkReply(client.get("/widgets?name=alpha&group=one"), 200).size() == 1);
    CHECK(checkReply(client.get("/widgets?group=one&limit=1&offset=1"), 200).size() == 1);

    const auto updated = checkReply(client.patch(path, R"({"name":"gamma"})"), 200);
    REQUIRE(updated.size() == 1);
    CHECK(updated[0]["name"].asString() == "gamma");
    CHECK(updated[0]["note"].asString() == "keep");
    CHECK(checkReply(client.remove(path), 200).empty());
    checkReply(client.get(path), 404);
}

TEST_CASE("CRUD routes reject invalid requests and roll back conflicting patches", "[crud]") {
    using namespace crud_test;
    const CrudClient client("/errors");
    const auto path = createWidget(client);
    checkReply(client.post("/widgets", R"({"name":"beta","group":"one"})"), 201);

    checkReply(client.post("/widgets", R"({"name":"alpha","group":"one"})"), 409);
    checkReply(client.get("/widgets?limit=101"), 400);
    checkReply(client.get("/widgets?private_value=private"), 400);
    checkReply(client.get("/widgets/not-an-id"), 400);
    checkReply(client.patch(path, R"({"name":null})"), 400);
    checkReply(client.patch(path, R"({"private_value":"leak"})"), 400);
    checkReply(client.patch(path, "{"), 400);
    checkReply(client.patch(path, R"({"name":"beta","note":null})"), 409);

    const auto item = checkReply(client.get(path), 200);
    REQUIRE(item.size() == 1);
    CHECK(item[0]["name"].asString() == "alpha");
    CHECK(item[0]["note"].asString() == "keep");
}

TEST_CASE("CRUD routes handle empty results, explicit null and disabled methods", "[crud]") {
    using namespace crud_test;
    const CrudClient client("/edges");
    const auto path = createWidget(client);

    CHECK(checkReply(client.get("/widgets?name=alpha&group=two"), 200).empty());
    const auto cleared = checkReply(client.patch(path, R"({"note":null})"), 200);
    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0]["note"].isNull());

    const auto disabledPost = client.post("/readonly", "{}");
    checkReply(disabledPost, 405);
    CHECK(disabledPost.header.at("allow") == "GET");

    const auto disabledPatch = client.patch("/readonly" + path.substr(path.rfind('/')), "{}");
    checkReply(disabledPatch, 405);
    CHECK(disabledPatch.header.at("allow") == "GET");

    checkReply(client.remove(path), 200);
    checkReply(client.patch(path, "{}"), 404);
}
