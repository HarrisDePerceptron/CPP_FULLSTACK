#pragma once

#include <trantor/net/EventLoop.h>

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace hello {

class WorkersBusy : public std::runtime_error {
public:
    WorkersBusy() : std::runtime_error("worker queue is full or closed") {}
};

class Workers {
public:
    Workers() {
        try {
            for (int index = 0; index < 4; ++index) {
                threads_.emplace_back([this] { run(); });
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    ~Workers() { stop(); }

    template<class Work>
    auto submit(Work work) {
        return Awaiter<Work>{*this, std::move(work), std::nullopt, {}};
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
        for (auto& thread : threads_) thread.join();
        threads_.clear();
    }

private:
    template<class Work>
    struct Awaiter {
        Workers& workers;
        Work work;
        std::optional<std::invoke_result_t<Work>> result;
        std::exception_ptr error;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> continuation) {
            auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
            if (!loop) throw std::logic_error("worker await requires an event loop");
            workers.enqueue([this, loop, continuation] {
                try {
                    result.emplace(work());
                } catch (...) {
                    error = std::current_exception();
                }
                loop->queueInLoop([continuation] { continuation.resume(); });
            });
        }

        auto await_resume() {
            if (error) std::rethrow_exception(error);
            return std::move(*result);
        }
    };

    void enqueue(std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            if (closed_ || pending_.size() >= 16) throw WorkersBusy{};
            pending_.push(std::move(job));
        }
        ready_.notify_one();
    }

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return closed_ || !pending_.empty(); });
                if (pending_.empty()) return;
                job = std::move(pending_.front());
                pending_.pop();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> pending_;
    bool closed_ = false;
    std::vector<std::thread> threads_;
};

}
