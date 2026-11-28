#pragma once

#include "server.h"
#include "framework/auth/tokens.h"
#include "framework/support/workers.h"

namespace APP_NAMESPACE {

class ServerRuntime {
  public:
    ServerRuntime(ResolvedServerConfig config, DatabaseBackendPtr database,
                  Logger log, Tokens tokens)
        : config_(std::move(config)), database_(std::move(database)),
          log_(std::move(log)), tokens_(std::move(tokens)),
          uploads_(config_.uploads_directory, config_.max_upload_bytes) {}

    const ResolvedServerConfig& config() const { return config_; }
    const DatabaseBackendPtr& database() const { return database_; }
    const Logger& log() const { return log_; }
    const Tokens& tokens() const { return tokens_; }
    const Uploads& uploads() const { return uploads_; }
    Workers& workers() const { return workers_; }

  private:
    ResolvedServerConfig config_;
    DatabaseBackendPtr database_;
    Logger log_;
    Tokens tokens_;
    Uploads uploads_;
    mutable Workers workers_;
};

}
