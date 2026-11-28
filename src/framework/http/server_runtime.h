#pragma once

#include "server.h"
#include "route_registrar.h"
#include "framework/auth/tokens.h"
#include "framework/support/log.h"
#include "framework/support/workers.h"

#include <utility>

namespace APP_NAMESPACE {

class ServerRuntime {
  public:
    ServerRuntime(drogon::HttpAppFramework& server, ResolvedServerConfig config,
                  Logger log, Tokens tokens)
        : server_(server), config_(std::move(config)), log_(std::move(log)),
          tokens_(std::move(tokens)),
          uploads_(config_.uploads_directory, config_.max_upload_bytes),
          routes_(server) {}

    drogon::HttpAppFramework& server() { return server_; }
    template <RoutePath Prefix = ""> auto routes() {
        return RouteRegistrar<Prefix>(routes_);
    }

    const ResolvedServerConfig& config() const { return config_; }
    const Logger& log() const { return log_; }
    const Tokens& tokens() const { return tokens_; }
    const Uploads& uploads() const { return uploads_; }
    template <typename Work> auto run_blocking(Work work) const {
        return workers_.run_blocking(std::move(work));
    }

    Workers& workers() const { return workers_; }

  private:
    drogon::HttpAppFramework& server_;
    ResolvedServerConfig config_;
    Logger log_;
    Tokens tokens_;
    Uploads uploads_;
    detail::RouteTable routes_;
    mutable Workers workers_;
};

}
