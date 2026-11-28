#include "server.h"

#include "server_hooks.h"
#include "server_runtime.h"
#include "framework/auth/auth.h"
#include "framework/support/validation.h"
#include "framework/support/workers.h"

#include <arpa/inet.h>
#include <drogon/HttpAppFramework.h>
#include <sodium.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t multipart_allowance = 64 * 1024;

bool valid_segment(std::string_view segment) {
    constexpr std::string_view characters =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    return !segment.empty() && segment != "." && segment != ".." &&
           segment.find_first_not_of(characters) == std::string_view::npos;
}

bool valid_url_segment(std::string_view segment) {
    return valid_segment(segment) && !segment.contains('.');
}

void validate_url_prefix(std::string_view prefix) {
    if (prefix.size() > 2 && prefix.starts_with('/') && prefix.ends_with('/')) {
        prefix.remove_prefix(1);
        while (!prefix.empty()) {
            const auto end = prefix.find('/');
            if (!valid_url_segment(prefix.substr(0, end)))
                break;
            prefix.remove_prefix(end + 1);
        }
        if (prefix.empty())
            return;
    }
    throw std::runtime_error("server.public_url_prefix must be a literal path "
                             "with leading and trailing slashes");
}

fs::path resolve_directory(const fs::path& root, const std::string& value,
                           std::string_view name) {
    if (value.empty() || value.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string(name) +
                                 " must be a directory path");
    }
    auto path = fs::weakly_canonical(root / value);
    if (path != path.root_path() && path.filename().empty())
        path = path.parent_path();
    return path;
}

bool contains_path(const fs::path& directory, const fs::path& path) {
    return std::mismatch(directory.begin(), directory.end(), path.begin(),
                         path.end())
               .first == directory.end();
}

bool contains_existing_directory(const fs::path& directory, fs::path path) {
    for (;;) {
        if (fs::equivalent(directory, path))
            return true;
        if (path == path.root_path())
            return false;
        path = path.parent_path();
    }
}

fs::path resolve_child(const fs::path& directory, const std::string& name) {
    const auto path = directory / name;
    if (fs::is_symlink(fs::symlink_status(path))) {
        throw std::runtime_error(
            "server storage entries must resolve to their configured paths");
    }
    return path;
}

ResolvedServerConfig resolve_config(const fs::path& root,
                                    const ServerConfig& config) {
    validate_url_prefix(config.public_url_prefix);
    if (!valid_segment(config.database_filename) ||
        !valid_url_segment(config.uploads_subdirectory) ||
        !valid_segment(config.request_cache_subdirectory)) {
        throw std::runtime_error("server database, uploads, and request cache "
                                 "names must be single path components");
    }
    require_positive_number(config.max_upload_bytes,
                            std::numeric_limits<std::size_t>::max() -
                                multipart_allowance,
                            "server.max_upload_bytes");

    const auto app_root = fs::canonical(root);
    const auto state = resolve_directory(app_root, config.state_directory,
                                         "server.state_directory");
    const auto public_directory = resolve_directory(
        app_root, config.public_directory, "server.public_directory");
    if (contains_path(public_directory, app_root) ||
        contains_path(public_directory, state) ||
        contains_path(state, public_directory)) {
        throw std::runtime_error(
            "server public and private directories must be separate");
    }
    const auto database = resolve_child(state, config.database_filename);
    const auto document_root = resolve_child(state, "empty");
    const auto cache = resolve_child(state, config.request_cache_subdirectory);
    if (database == document_root || cache == document_root ||
        database == cache) {
        throw std::runtime_error("server database, document root, and request "
                                 "cache paths must be distinct");
    }

    return {database,
            document_root,
            cache,
            public_directory,
            resolve_child(public_directory, config.uploads_subdirectory),
            config.public_url_prefix,
            config.public_url_prefix + config.uploads_subdirectory + '/',
            config.max_upload_bytes,
            config.max_upload_bytes + multipart_allowance};
}

void register_shutdown(drogon::HttpAppFramework& server,
                       std::weak_ptr<ServerRuntime> runtime) {
    auto stopping = std::make_shared<std::atomic_flag>();
    const auto request_stop = [stopping] { stopping->test_and_set(); };
    server.setTermSignalHandler(request_stop).setIntSignalHandler(request_stop);
    server.registerBeginningAdvice(
        [&server, stopping, runtime = std::move(runtime)] {
            server.getLoop()->runEvery(0.1, [&server, stopping, runtime] {
                if (stopping->test()) {
                    if (const auto running = runtime.lock())
                        running->workers().stop();
                    server.quit();
                }
            });
        });
}

}

void ServerConfig::validate() const {
    in_addr parsed{};
    if (bind_address.find('\0') != std::string::npos ||
        inet_pton(AF_INET, bind_address.c_str(), &parsed) != 1) {
        throw std::runtime_error("bind_address must be an IPv4 address");
    }
    require_positive_number(port, 65535, "port");
    require_positive_number(max_connections,
                            std::numeric_limits<std::size_t>::max(),
                            "max_connections");
    require_positive_number(jwt_lifetime_seconds, 86400,
                            "server.jwt_lifetime_seconds");
    parse_log_level(log_level);
}

int run_http_server(drogon::HttpAppFramework& server, const fs::path& root,
                    const ServerConfig& config, const ServerSetup& setup) {
    config.validate();
    const auto resolved = resolve_config(root, config);
    if (sodium_init() < 0)
        throw std::runtime_error("could not initialize libsodium");

    fs::create_directories(resolved.document_root);
    fs::create_directories(resolved.request_cache_directory);
    fs::create_directories(resolved.uploads_directory);

    // Check filesystem identity before mounting static files, including
    // case-insensitive aliases.
    const auto state = resolved.database_path.parent_path();
    if (contains_existing_directory(resolved.public_directory,
                                    fs::canonical(root)) ||
        contains_existing_directory(resolved.public_directory, state) ||
        contains_existing_directory(state, resolved.public_directory)) {
        throw std::runtime_error(
            "server public and private directories must be separate");
    }

    const auto log_level = parse_log_level(config.log_level);
    const Logger log(log_level);
    Tokens tokens(load_signing_key(),
                  std::chrono::seconds(config.jwt_lifetime_seconds));
    const auto runtime = std::make_shared<ServerRuntime>(server, resolved, log,
                                                         std::move(tokens));
    server
        .addListener(config.bind_address,
                     static_cast<std::uint16_t>(config.port))
        .setDocumentRoot(resolved.document_root.string())
        .setUploadPath(resolved.request_cache_directory.string())
        .setImplicitPageEnable(false)
        .setClientMaxBodySize(resolved.max_request_bytes)
        .setMaxConnectionNum(config.max_connections)
        .setFileTypes({"html", "css", "js", "png", "jpg", "jpeg", "gif", "webp",
                       "pdf", "svg"})
        .addALocation(resolved.public_url_prefix, "",
                      resolved.public_directory.string(), true, false, false);

    register_access_logging(server, log_level, log, resolved.public_url_prefix);
    register_exception_handler(server, log);
    {
        trantor::EventLoopThread startup_thread;
        startup_thread.run();
        const auto initialize = [&]() -> drogon::Task<> {
            co_await drogon::switchThreadCoro(startup_thread.getLoop());
            co_await setup(*runtime);
        };
        drogon::sync_wait(initialize());
    }
    const detail::AuthVerifierScope auth_verifier(
        {runtime, &runtime->tokens()});
    server.registerBeginningAdvice([log, address = config.bind_address,
                                    port = config.port] {
        log.write(LogLevel::info, "Listening on http://{}:{}", address, port);
        std::fflush(stdout);
    });
    register_shutdown(server, runtime);
    server.run();
    runtime->workers().stop();
    return 0;
}

}
