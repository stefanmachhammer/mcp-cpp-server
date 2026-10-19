#include "mcp/dispatcher.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace mcp {

std::optional<std::string> Dispatcher::run(Step step, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<std::string>>();
    std::future<std::string> future = promise->get_future();
    {
        std::lock_guard lock(mutex_);
        if (!open_) return std::nullopt;
        jobs_.push_back({std::move(step), promise});
    }
    if (future.wait_for(timeout) != std::future_status::ready) return std::nullopt;
    try {
        return future.get();
    } catch (const std::exception&) {
        return std::nullopt;  // shutdown broke the promise
    }
}

void Dispatcher::pump(std::chrono::milliseconds budget) {
    const auto start = std::chrono::steady_clock::now();
    std::size_t rounds;
    {
        std::lock_guard lock(mutex_);
        rounds = jobs_.size();  // each queued job gets at most one step per frame
    }
    while (rounds-- > 0) {
        Job job;
        {
            std::lock_guard lock(mutex_);
            if (jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::optional<std::string> result;
        try {
            result = job.step();
        } catch (const std::exception& e) {
            result = std::string("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,"
                                 "\"message\":\"") + e.what() + "\"}}";
        } catch (...) {
            result = std::string("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,"
                                 "\"message\":\"unknown exception\"}}");
        }
        constexpr int kMaxAttempts = 1200;  // a multi-frame tool that never finishes
        if (!result && ++job.attempts >= kMaxAttempts)
            result = std::string("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,"
                                 "\"message\":\"tool did not complete\"}}");
        if (result) {
            job.promise->set_value(std::move(*result));
        } else {
            std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        if (std::chrono::steady_clock::now() - start > budget) return;
    }
}

void Dispatcher::shutdown() {
    std::deque<Job> drop;
    {
        std::lock_guard lock(mutex_);
        open_ = false;
        drop.swap(jobs_);
    }
    for (Job& j : drop) {
        try {
            j.promise->set_exception(
                std::make_exception_ptr(std::runtime_error("MCP server stopped")));
        } catch (...) {
        }
    }
}

void Dispatcher::reopen() {
    std::lock_guard lock(mutex_);
    open_ = true;
}

std::size_t Dispatcher::pending() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}

}  // namespace mcp
