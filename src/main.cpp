#include <vector>
#include <thread>
#include <boost/asio.hpp>
#include <CLI/CLI.hpp>
#include "std.hpp"
#include "Logger.hpp"

namespace asio = boost::asio;
using asio::awaitable;
using asio::use_awaitable;
using asio::ip::tcp;

awaitable<void> connection(tcp::socket socket) {
    char buf[4096];
    for (;;) {
        std::size_t n = co_await socket.async_read_some(asio::buffer(buf), use_awaitable);
        co_await asio::async_write(socket, asio::buffer(buf, n), use_awaitable);
    }
}

awaitable<void> listener(tcp::acceptor& acceptor) {
    asio::steady_timer backoff(acceptor.get_executor());
    for (;;) {
        // each connection gets its own strand, so its handlers never run concurrently
        auto [ec, socket] = co_await acceptor.async_accept(
            asio::make_strand(acceptor.get_executor()), 
            asio::as_tuple(use_awaitable));
        if (!ec) {
            // srand executor, you can assign tasks to it to prevent multiple thread access to the socket
            auto srand_ex = socket.get_executor();
            // start task and don't wait
            asio::co_spawn(srand_ex, connection(std::move(socket)), [](auto e) {
                if (!e) return; // operation aborted
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
        CLI::App app("servecpp - async web server");
        std::string address_str = "127.0.0.1";
        u16 port = 8080;
        app.add_option("-a,--address", address_str, "address to listen on")
            ->capture_default_str();
        app.add_option("-p,--port", port, "port to listen on")
            ->capture_default_str()
            ->check(CLI::Range(1, 65535));
        // handles --help and parse errors: prints the help or the error and returns from main
        CLI11_PARSE(app, argc, argv);
        // accepts both IPv4 and IPv6, throws on invalid address
        auto address = asio::ip::make_address(address_str);

        // create as many threads as there are CPU logical cores
        // or at least 1 if hardware_concurrency() fails
        i32 thread_n = std::max(1u, std::thread::hardware_concurrency());
        asio::io_context ctx(thread_n);
        
        tcp::acceptor acceptor(ctx, {address, port});
        asio::co_spawn(ctx, listener(acceptor), [&ctx](auto e) {
            if (!e) return; // operation aborted
            log_error("Error in listener: {}", stringify_exception(e));
            ctx.stop();
        });

        asio::signal_set signals(ctx, SIGINT, SIGTERM);
        // wait infinitely until stop signal is received
        signals.async_wait([&ctx](auto, auto) { ctx.stop(); });

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
