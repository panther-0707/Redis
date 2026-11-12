#include "miniLB/util/Logger.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace minilb {

void initLogger(const std::string& level) {
    // "_mt" = multi-threaded: one mutex inside the sink keeps lines from
    // different threads from interleaving.
    auto logger = spdlog::stdout_color_mt("minilb");
    spdlog::set_default_logger(logger);

    // Example: [2026-01-31 12:00:00.123] [info] [t 4242] message
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t %t] %v");
    spdlog::set_level(spdlog::level::from_str(level));

    // Write each line out immediately so nothing is lost on a crash.
    spdlog::flush_on(spdlog::level::trace);
}

} // namespace minilb
