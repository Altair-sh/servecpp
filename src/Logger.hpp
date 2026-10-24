#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <format>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

enum class LogLevel { debug, info, warning, error };

/// Thread-safe logger for async code.
/// log_*() formats the message on the calling thread and only appends it to a queue,
/// a background thread writes queued lines, so I/O threads never wait for the console.
class Logger {
public:
    /// format string checked at compile time + location of the log_*() call
    template <class... Args>
    struct FormatWithLocation {
        std::format_string<Args...> fmt;
        std::source_location loc;

        template <class S>
        consteval FormatWithLocation(const S& s, std::source_location loc = std::source_location::current())
            : fmt(s), loc(loc) {}
    };
    template <class... Args>
    using Format = FormatWithLocation<std::type_identity_t<Args>...>;

    explicit Logger(std::FILE* out = stdout, LogLevel min_level = LogLevel::debug);
    /// writes remaining messages and stops the writer thread
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept { min_level.store(level, std::memory_order_relaxed); }

    /// thin template: only packs arguments, the formatting is done once in vlog()
    template <class... Args>
    void log(LogLevel level, Format<Args...> f, Args&&... args) noexcept {
        if (level < min_level.load(std::memory_order_relaxed))
            return;
        vlog(level, f.loc, f.fmt.get(), std::make_format_args(args...));
    }

private:
    std::FILE* out;
    std::atomic<LogLevel> min_level;
    std::mutex queue_mutex;
    std::condition_variable_any queue_cv;
    std::vector<std::string> queue;
    std::jthread writer;  // declared last: starts after the other members are initialized

    void vlog(LogLevel level, const std::source_location& loc, std::string_view fmt, std::format_args args) noexcept;
    void enqueue(std::string line);
    void writer_loop(std::stop_token stop);
};

/// process-wide logger, created on first use, writes remaining messages at program exit
Logger& logger();

/// log_*() write to the process-wide logger()
template <class... Args>
void log_debug(Logger::Format<Args...> f, Args&&... args) noexcept {
    logger().log(LogLevel::debug, f, std::forward<Args>(args)...);
}
template <class... Args>
void log_info(Logger::Format<Args...> f, Args&&... args) noexcept {
    logger().log(LogLevel::info, f, std::forward<Args>(args)...);
}
template <class... Args>
void log_warning(Logger::Format<Args...> f, Args&&... args) noexcept {
    logger().log(LogLevel::warning, f, std::forward<Args>(args)...);
}
template <class... Args>
void log_error(Logger::Format<Args...> f, Args&&... args) noexcept {
    logger().log(LogLevel::error, f, std::forward<Args>(args)...);
}

/// Works with exceptions thrown by BOOST_THROW_EXCEPTION()
/// @return e.what + source_location
std::string stringify_exception(const std::exception& e);
std::string stringify_exception(std::exception_ptr ep);
