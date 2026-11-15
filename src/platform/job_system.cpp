#include "slrr/job_system.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace slrr {
namespace {

// Set on workers, and on the calling thread while it runs bodies, so a
// nested call runs serially instead of waiting for itself.
thread_local bool t_in_job = false;

struct Batch {
  size_t count = 0;
  const std::function<void(size_t)>* body = nullptr;
  std::atomic_size_t next{0};
  std::atomic_size_t finished{0};
  std::mutex error_mu;
  std::exception_ptr error;
};

size_t logical_processors() {
  size_t count = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
  // A process restricted to some processors (start /affinity, Task Manager)
  // should not oversubscribe them.
  DWORD_PTR process = 0, system = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
    count = std::min<size_t>(count, static_cast<size_t>(std::popcount(
                                        static_cast<uint64_t>(process))));
#endif
  return std::max<size_t>(1, count);
}

void name_worker(size_t index) {
#ifdef _WIN32
  // SetThreadDescription exists from Windows 10 1607; look it up so older
  // systems still start. The name shows in debuggers and profilers.
  using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const auto set = reinterpret_cast<SetDescription>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (!set) return;
  const std::wstring name = L"slrr worker " + std::to_wstring(index);
  set(GetCurrentThread(), name.c_str());
#else
  (void)index;
#endif
}

void drain(Batch& batch, std::mutex& mu, std::condition_variable& done) {
  for (size_t i = batch.next.fetch_add(1); i < batch.count; i = batch.next.fetch_add(1)) {
    try {
      (*batch.body)(i);
    } catch (...) {
      std::lock_guard lock(batch.error_mu);
      if (!batch.error) batch.error = std::current_exception();
    }
    if (batch.finished.fetch_add(1) + 1 == batch.count) {
      // Under the lock, so the caller cannot miss it between its check and
      // its wait.
      std::lock_guard lock(mu);
      done.notify_all();
    }
  }
}

class Pool {
 public:
  ~Pool() { stop(); }

  size_t concurrency() {
    start();
    std::lock_guard lock(mu_);
    return workers_.size() + 1;
  }

  void run(size_t count, const std::function<void(size_t)>& body) {
    if (count == 0) return;
    start();
    std::unique_lock submit(submit_mu_, std::defer_lock);
    // One batch at a time. A nested call, or a second thread's call while a
    // batch runs, does its own work instead of waiting for the pool.
    if (count == 1 || t_in_job || !submit.try_lock() || !has_workers()) {
      std::exception_ptr error;
      for (size_t i = 0; i < count; ++i) {
        try {
          body(i);
        } catch (...) {
          if (!error) error = std::current_exception();
        }
      }
      if (error) std::rethrow_exception(error);
      return;
    }
    auto batch = std::make_shared<Batch>();
    batch->count = count;
    batch->body = &body;
    {
      std::lock_guard lock(mu_);
      batch_ = batch;
      ++generation_;
    }
    wake_.notify_all();
    t_in_job = true;
    drain(*batch, mu_, done_);
    t_in_job = false;
    {
      // Workers that claimed an index finish it before the batch counts as
      // done; one that wakes later finds no index left and never calls body.
      std::unique_lock lock(mu_);
      done_.wait(lock, [&] { return batch->finished.load() == count; });
      batch_.reset();
    }
    if (batch->error) std::rethrow_exception(batch->error);
  }

  void stop() {
    std::vector<std::jthread> workers;
    {
      std::lock_guard lock(mu_);
      workers.swap(workers_);
      stopped_ = true;
    }
    for (auto& worker : workers) worker.request_stop();
    workers.clear();  // joins
  }

 private:
  bool has_workers() {
    std::lock_guard lock(mu_);
    return !workers_.empty();
  }

  void start() {
    std::call_once(started_, [this] {
      const size_t wanted = logical_processors() - 1;
      std::lock_guard lock(mu_);
      if (stopped_) return;
      try {
        for (size_t i = 0; i < wanted; ++i)
          workers_.emplace_back([this, i](std::stop_token stop) { work(stop, i + 1); });
      } catch (const std::system_error&) {
        // Run with the workers that started; the caller always takes part.
      }
      std::printf("[jobs] %zu worker threads + host thread\n", workers_.size());
    });
  }

  void work(std::stop_token stop, size_t index) {
    name_worker(index);
    t_in_job = true;
    uint64_t seen = 0;
    std::unique_lock lock(mu_);
    while (true) {
      wake_.wait(lock, stop, [&] { return generation_ != seen; });
      if (stop.stop_requested()) return;
      seen = generation_;
      const auto batch = batch_;
      if (!batch) continue;
      lock.unlock();
      drain(*batch, mu_, done_);
      lock.lock();
    }
  }

  std::once_flag started_;
  std::mutex submit_mu_;
  std::mutex mu_;
  std::condition_variable_any wake_;
  std::condition_variable done_;
  std::shared_ptr<Batch> batch_;
  uint64_t generation_ = 0;
  bool stopped_ = false;
  std::vector<std::jthread> workers_;
};

Pool& pool() {
  static Pool instance;
  return instance;
}

}  // namespace

size_t jobs_concurrency() { return pool().concurrency(); }

void jobs_parallel_for(size_t count, const std::function<void(size_t)>& body) {
  pool().run(count, body);
}

void jobs_shutdown() { pool().stop(); }

}  // namespace slrr
