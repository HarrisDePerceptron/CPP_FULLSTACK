#pragma once

#include "app_namespace.hpp"

#include "config.hpp"

#include <chrono>
#include <format>
#include <print>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

class Logger {
public:
    explicit Logger(LogLevel minimum) : minimum_(minimum) {}

    template<class... Args>
    void write(LogLevel level, std::format_string<Args...> format, Args&&... args) const {
        if (level < minimum_) return;
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
