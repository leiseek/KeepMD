#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
namespace keepmd {
class Worker {
  public:
    Worker()
        : thread_([this](std::stop_token stop) {
              for (;;) {
                  std::function<void()> job;
                  {
                      std::unique_lock lock(mutex_);
                      ready_.wait(lock, [&] { return stop.stop_requested() || !jobs_.empty(); });
                      if (stop.stop_requested())
                          return;
                      job = std::move(jobs_.front());
                      jobs_.pop_front();
                  }
                  try {
                      job();
                  } catch (...) { /* Individual jobs report errors; thread stays alive. */
                  }
              }
          }) {}
    ~Worker() {
        thread_.request_stop();
        clear();
        ready_.notify_all();
    }
    bool push(std::function<void()> job) {
        std::lock_guard lock(mutex_);
        if (jobs_.size() >= 4)
            return false;
        jobs_.push_back(std::move(job));
        ready_.notify_one();
        return true;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        jobs_.clear();
    }

  private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    std::jthread thread_;
};
} // namespace keepmd
