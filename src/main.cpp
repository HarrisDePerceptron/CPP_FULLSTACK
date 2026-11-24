#include "server.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>

int main() {
    try {
        return hello::serve(std::filesystem::current_path());
    } catch (const std::exception& error) {
        std::println(stderr, "Startup failed: {}", error.what());
        return 1;
    }
}
