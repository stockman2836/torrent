#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>

namespace torrent {

class Logger {
public:
    enum class Level {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR,
        CRITICAL,
        OFF
    };

    static void init(const std::string& log_file = "torrent_client.log",
                     Level console_level = Level::INFO,
                     Level file_level = Level::DEBUG,
                     size_t max_file_size = 1024 * 1024 * 5,  // 5 MB
                     size_t max_files = 3);

    static void setLevel(Level level);
    static void setConsoleLevel(Level level);
    static void setFileLevel(Level level);

    static std::shared_ptr<spdlog::logger> get();

private:
    static std::shared_ptr<spdlog::logger> logger_;
    static spdlog::level::level_enum toSpdlogLevel(Level level);
};

} // namespace torrent

// Convenient logging macros
#define LOG_TRACE(...)    if (torrent::Logger::get()) torrent::Logger::get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...)    if (torrent::Logger::get()) torrent::Logger::get()->debug(__VA_ARGS__)
#define LOG_INFO(...)     if (torrent::Logger::get()) torrent::Logger::get()->info(__VA_ARGS__)
#define LOG_WARN(...)     if (torrent::Logger::get()) torrent::Logger::get()->warn(__VA_ARGS__)
#define LOG_ERROR(...)    if (torrent::Logger::get()) torrent::Logger::get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) if (torrent::Logger::get()) torrent::Logger::get()->critical(__VA_ARGS__)
