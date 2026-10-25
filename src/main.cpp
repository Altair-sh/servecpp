#include <filesystem>
#include <vector>
#include <thread>
#include <boost/asio.hpp>
#include <CLI/CLI.hpp>
#include "std.hpp"
#include "Config.hpp"
#include "HttpSession.hpp"
#include "Logger.hpp"
#include "Path.hpp"
#include "TempFile.hpp"

namespace asio = boost::asio;
namespace fs = std::filesystem;
using asio::awaitable;
using asio::use_awaitable;
using asio::ip::tcp;

awaitable<void> listener(tcp::acceptor& acceptor, const Config& config) {
    asio::steady_timer backoff(acceptor.get_executor());
    for (;;) {
        // each connection gets its own strand, so its handlers never run concurrently
        auto [ec, socket] = co_await acceptor.async_accept(
            asio::make_strand(acceptor.get_executor()),
            asio::as_tuple(use_awaitable));
        if (!ec) {
            // strand executor, you can assign tasks to it to prevent multiple thread access to the socket
            auto strand_ex = socket.get_executor();
            // start task and don't wait
            asio::co_spawn(strand_ex, http_session(std::move(socket), config), [](auto e) {
                if (e)
                    log_error("Error in connection: {}", stringify_exception(e));
            });
            continue;
        }
        // acceptor closed: shutting down
        if (ec == asio::error::operation_aborted)
            co_return;
        // client stopped, not our problem
        if (ec == asio::error::connection_aborted || ec == asio::error::connection_reset)
            continue;
        // out of resources: wait instead of spinning, then retry
        if (ec == asio::error::no_descriptors || ec == asio::error::no_buffer_space || ec == asio::error::no_memory) {
            log_warning("async_accept: {}, retrying", ec.message());
            backoff.expires_after(std::chrono::milliseconds(100));
            co_await backoff.async_wait(use_awaitable);
            continue;
        }
        // other error code
        throw boost::system::system_error(ec, "async_accept");
    }
}

i32 main(i32 argc, char* argv[]) {
    try {
        CLI::App app("servecpp - HTTP/1.1 file server");
        std::string config_path = "./servecpp.toml";
        app.add_option("-c,--config", config_path, "path to the config file")
            ->capture_default_str();
        // handles --help and parse errors: prints the help or the error and returns from main
        CLI11_PARSE(app, argc, argv);

        // argv is in the native encoding (ANSI code page on Windows), so is fs::path(std::string)
        fs::path config_file(config_path);
        std::error_code fs_ec;
        if (!fs::exists(config_file, fs_ec)) {
            log_error("config file '{}' not found", config_path);
            return 1;
        }
        Config config = load_config(config_file);
        remove_unfinished_uploads(config);

        // create as many threads as there are CPU logical cores
        // or at least 1 if hardware_concurrency() fails
        i32 thread_n = std::max(1u, std::thread::hardware_concurrency());
        asio::io_context ctx(thread_n);

        tcp::acceptor acceptor(ctx, { config.address, config.port });
        asio::co_spawn(ctx, listener(acceptor, config), [&ctx](auto e) {
            if (!e) return; // operation aborted
            log_error("Error in listener: {}", stringify_exception(e));
            ctx.stop();
        });
        log_info("listening on {}:{}", config.address.to_string(), config.port);
        for (const Share& share : config.shares)
            log_info("share {} -> {}", share.url, path_utf8(share.path));

        asio::signal_set signals(ctx, SIGINT, SIGTERM);
        // wait infinitely until stop signal is received.
        // Running transfers are cut off: unfinished uploads remove their temp files
        // when the io_context destroys the sessions.
        signals.async_wait([&ctx](auto, auto) {
            log_info("stopping");
            ctx.stop();
        });

        // jthreads join on destruction
        std::vector<std::jthread> thread_pool;
        thread_pool.reserve(thread_n - 1);
        for (i32 i = 0; i < thread_n - 1; i++)
            thread_pool.emplace_back([&ctx] { ctx.run(); });
        // the main thread works too
        ctx.run();
        // exit happens only on termination signal
        return 0;
    }
    catch (const std::exception& e) {
        log_error("Error in main: {}", stringify_exception(e));
    }
    catch (...) {
        log_error("Error in main: unknown error");
    }
    return 1;
}
