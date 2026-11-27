#pragma once

#include "app_namespace.h"

#include <chrono>
#include <format>
#include <print>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

enum class LogLevel { info,
                      warn,
                      error };

inline LogLevel parse_log_level(std::string_view value) {
    if (value == "info")
        return LogLevel::info;
    if (value == "warn")
        return LogLevel::warn;
    if (value == "error")
        return LogLevel::error;
    throw std::runtime_error("log_level must be info, warn, or error");
}

class Logger {
  public:
    explicit Logger(LogLevel minimum) : minimum_(minimum) {}

    template <class... Args>
    void write(LogLevel level, std::format_string<Args...> format, Args&&... args) const {
        if (level < minimum_)
            return;
        constexpr std::string_view labels[]{"INFO", "WARN", "ERROR"};
        const auto now = std::chrono::floor<std::chrono::milliseconds>(
            std::chrono::system_clock::now());
        std::println("{:%FT%T}Z [{}] {}", now, labels[static_cast<int>(level)],
                     std::format(format, std::forward<Args>(args)...));
    }

  private:
    LogLevel minimum_;
};

}
