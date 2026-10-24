#include "Logger.hpp"

#include <chrono>
#include <iterator>
#include <boost/throw_exception.hpp>
#include "std.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

Logger::Logger(std::FILE* out, LogLevel min_level)
    : out(out), min_level(min_level), writer([this](std::stop_token stop) { writer_loop(stop); })
{
#ifdef _WIN32
    // console interprets written bytes as UTF-8 (default is the legacy OEM code page)
    SetConsoleOutputCP(CP_UTF8);
#endif
}

Logger::~Logger() {
    writer.request_stop();
    writer.join();
}

void Logger::vlog(LogLevel level, const std::source_location& loc, std::string_view fmt, std::format_args args) noexcept {
    try {
        static constexpr std::string_view level_names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

        std::string line;
        line.reserve(256);
        
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::format_to(std::back_inserter(line), "[{:%F %T}][{}] ",
            now, level_names[static_cast<int>(level)]);

        if (level != LogLevel::info) {
            std::string_view file = loc.file_name();
            file.remove_prefix(file.find_last_of("/\\") + 1);  // npos + 1 == 0 keeps the whole name
            std::format_to(std::back_inserter(line), "{}:{}: ", file, loc.line());
        }

        std::vformat_to(std::back_inserter(line), fmt, args);
        line += '\n';
        enqueue(std::move(line));
    }
    catch (...) {
        // logging must never throw, a message lost on bad_alloc is acceptable
    }
}

void Logger::enqueue(std::string line) {
    {
        std::lock_guard lock(queue_mutex);
        queue.push_back(std::move(line));
    }
    queue_cv.notify_one();
}

void Logger::writer_loop(std::stop_token stop) {
    std::vector<std::string> batch;
    for (;;) {
        {
            std::unique_lock lock(queue_mutex);
            // wakes up on a new message or on stop request
            queue_cv.wait(lock, stop, [this] { return !queue.empty(); });
            if (queue.empty())
                return;  // stop requested and everything is written
            batch.swap(queue);  // take all queued lines, producers are blocked only for the swap
        }
        for (const std::string& line : batch)
            std::fwrite(line.data(), 1, line.size(), out);
        std::fflush(out);
        batch.clear();
    }
}

Logger& logger() {
    static Logger instance;
    return instance;
}

std::string stringify_exception(const std::exception& e) {
    cstr msg = e.what();
    boost::source_location loc = boost::get_throw_location(e);
    if (loc.line() == 0)
        return msg;

    return std::format("{}\n    at {}:{} in {}",
        msg, loc.file_name(), loc.line(), loc.function_name());
}

std::string stringify_exception(std::exception_ptr ep) {
    try {
        // there is really no other way to get anything from exception_ptr in C++20
        std::rethrow_exception(ep);
    }
    catch (const std::exception& e) { return stringify_exception(e); }
    catch (...) { return "unknown exception"; }
}
