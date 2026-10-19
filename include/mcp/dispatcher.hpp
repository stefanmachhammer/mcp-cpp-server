#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mcp {

// Hands work from the network threads to the host's main thread. A step
// returns nullopt to be retried next frame, which lets a tool span frames
// (e.g. wait for a GPU readback) without ever blocking the main loop.
class Dispatcher {
public:
    using Step = std::function<std::optional<std::string>()>;

    // Network thread. Blocks until the main thread produced a result or the
    // timeout passed; on timeout the job keeps running and its result is dropped.
    std::optional<std::string> run(Step step, std::chrono::milliseconds timeout);

    // Main thread, once per frame. Executes queued steps until the budget is
    // spent; unfinished steps wait for the next frame.
    void pump(std::chrono::milliseconds budget);

    // Fails every waiting caller so network threads can unwind before stop.
    void shutdown();
    void reopen();

    std::size_t pending() const;

private:
    struct Job {
        Step                                       step;
        std::shared_ptr<std::promise<std::string>> promise;
        int                                        attempts = 0;
    };

    mutable std::mutex mutex_;
    std::deque<Job>    jobs_;
    bool               open_ = true;
};

}  // namespace mcp
