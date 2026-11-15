#include "standalone_test.hpp"

#include "slrr/job_system.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using slrr::jobs_concurrency;
using slrr::jobs_parallel_for;

SLRR_TEST(every_index_runs_exactly_once) {
  SLRR_REQUIRE(jobs_concurrency() >= 1);
  for (const size_t count : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{1000}}) {
    std::vector<std::atomic_int> hits(count);
    jobs_parallel_for(count, [&](size_t i) { hits[i].fetch_add(1); });
    for (const auto& hit : hits) SLRR_REQUIRE(hit.load() == 1);
  }
}

SLRR_TEST(work_spreads_over_several_threads) {
  if (jobs_concurrency() < 2) return;
  std::mutex mu;
  std::set<std::thread::id> threads;
  jobs_parallel_for(256, [&](size_t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::lock_guard lock(mu);
    threads.insert(std::this_thread::get_id());
  });
  SLRR_REQUIRE(threads.size() >= 2);
}

SLRR_TEST(results_are_visible_to_the_caller) {
  std::vector<size_t> squares(5000);
  jobs_parallel_for(squares.size(), [&](size_t i) { squares[i] = i * i; });
  for (size_t i = 0; i < squares.size(); ++i) SLRR_REQUIRE(squares[i] == i * i);
}

SLRR_TEST(nested_calls_run_serially_without_deadlock) {
  std::atomic_int total{0};
  jobs_parallel_for(16, [&](size_t) {
    const auto outer = std::this_thread::get_id();
    jobs_parallel_for(8, [&](size_t) {
      SLRR_REQUIRE(std::this_thread::get_id() == outer);
      total.fetch_add(1);
    });
  });
  SLRR_REQUIRE(total.load() == 16 * 8);
}

SLRR_TEST(first_exception_reaches_the_caller_after_every_index) {
  std::atomic_int ran{0};
  bool caught = false;
  try {
    jobs_parallel_for(64, [&](size_t i) {
      ran.fetch_add(1);
      if (i % 10 == 3) throw std::runtime_error("job failed");
    });
  } catch (const std::runtime_error&) {
    caught = true;
  }
  SLRR_REQUIRE(caught);
  SLRR_REQUIRE(ran.load() == 64);
  // The pool still works after a failed batch.
  std::atomic_int after{0};
  jobs_parallel_for(64, [&](size_t) { after.fetch_add(1); });
  SLRR_REQUIRE(after.load() == 64);
}

SLRR_TEST(concurrent_callers_both_complete) {
  std::atomic_int a{0}, b{0};
  std::thread other([&] {
    for (int round = 0; round < 50; ++round)
      jobs_parallel_for(100, [&](size_t) { b.fetch_add(1); });
  });
  for (int round = 0; round < 50; ++round)
    jobs_parallel_for(100, [&](size_t) { a.fetch_add(1); });
  other.join();
  SLRR_REQUIRE(a.load() == 5000);
  SLRR_REQUIRE(b.load() == 5000);
}

SLRR_TEST(shutdown_falls_back_to_the_calling_thread) {
  slrr::jobs_shutdown();
  const auto caller = std::this_thread::get_id();
  std::atomic_int total{0};
  jobs_parallel_for(32, [&](size_t) {
    SLRR_REQUIRE(std::this_thread::get_id() == caller);
    total.fetch_add(1);
  });
  SLRR_REQUIRE(total.load() == 32);
}

int main() {
  const slrr::standalone::Test tests[] = {
      every_index_runs_exactly_once,
      work_spreads_over_several_threads,
      results_are_visible_to_the_caller,
      nested_calls_run_serially_without_deadlock,
      first_exception_reaches_the_caller_after_every_index,
      concurrent_callers_both_complete,
      shutdown_falls_back_to_the_calling_thread,
  };
  return slrr::standalone::run("job_system", tests);
}
