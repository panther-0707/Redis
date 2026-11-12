#pragma once

// Logging goes through spdlog, which is thread-safe (the "_mt" loggers lock
// internally) and supports levels. Use it directly anywhere in the code:
//
//     spdlog::info("listening on port {}", port);
//     spdlog::warn("backend {} is down", id);
#include <spdlog/spdlog.h>

#include <string>

namespace minilb {

// Sets up the process-wide logger. `level` is one of:
// trace, debug, info, warn, error, critical, off.
void initLogger(const std::string& level = "info");

} // namespace minilb
