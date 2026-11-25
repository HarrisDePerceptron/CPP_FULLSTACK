#include "server/server.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>

int main() {
  try {

    return APP_NAMESPACE::serve(std::filesystem::current_path());

  } catch (const std::exception &error) {
    std::println(stderr, "Startup failed: {}", error.what());
    return 1;
  }
}
