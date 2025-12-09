#include "logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <vector>

namespace torrent {

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;

void Logger::init(const std::string& log_file,
                  Level console_level,
                  Level file_level,
                  size_t max_file_size,
                  size_t max_files) {
    try {
        std::vector<spdlog::sink_ptr> sinks;

        // Console sink (colored output)
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(toSpdlogLevel(console_level));
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(console_sink);

        // Rotating file sink
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, max_file_size, max_files);
        file_sink->set_level(toSpdlogLevel(file_level));
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        sinks.push_back(file_sink);

        // Create logger with both sinks
        logger_ = std::make_shared<spdlog::logger>("torrent_client", sinks.begin(), sinks.end());
        logger_->set_level(spdlog::level::trace);  // Set to lowest level, sinks control actual output
        logger_->flush_on(spdlog::level::warn);    // Auto-flush on warnings and errors

        // Register as default logger
        spdlog::set_default_logger(logger_);

        LOG_INFO("Logger initialized - Console: {}, File: {}",
                 spdlog::level::to_string_view(toSpdlogLevel(console_level)),
                 spdlog::level::to_string_view(toSpdlogLevel(file_level)));
    } catch (const spdlog::spdlog_ex& ex) {
        fprintf(stderr, "Logger initialization failed: %s\n", ex.what());
    }
}

void Logger::setLevel(Level level) {
    if (logger_) {
        logger_->set_level(toSpdlogLevel(level));
    }
}

void Logger::setConsoleLevel(Level level) {
    if (logger_ && logger_->sinks().size() > 0) {
        logger_->sinks()[0]->set_level(toSpdlogLevel(level));
    }
}

void Logger::setFileLevel(Level level) {
    if (logger_ && logger_->sinks().size() > 1) {
        logger_->sinks()[1]->set_level(toSpdlogLevel(level));
    }
}

std::shared_ptr<spdlog::logger> Logger::get() {
    return logger_;
}

spdlog::level::level_enum Logger::toSpdlogLevel(Level level) {
    switch (level) {
        case Level::TRACE:    return spdlog::level::trace;
        case Level::DEBUG:    return spdlog::level::debug;
        case Level::INFO:     return spdlog::level::info;
        case Level::WARN:     return spdlog::level::warn;
        case Level::ERROR:    return spdlog::level::err;
        case Level::CRITICAL: return spdlog::level::critical;
        case Level::OFF:      return spdlog::level::off;
        default:              return spdlog::level::info;
    }
}

} // namespace torrent
