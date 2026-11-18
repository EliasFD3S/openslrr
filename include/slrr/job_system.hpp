#pragma once

// Host worker pool for data-parallel native work: mesh and texture decoding,
// pack indexing and other loading steps that only read files and build
// plain C++ data.
//
// The VM, script fibers, GameRef state, GC objects and bgfx stay on the host
// thread (ENGINE_CONTRACT.md §6). A job body must not touch them: it fills a
// slot of a caller-owned result, and the caller publishes the results once
// jobs_parallel_for returns.

#include <cstddef>
#include <functional>
#include <string>

namespace slrr {

// Threads that run jobs_parallel_for work, the calling thread included: one
// per logical processor the process may run on, and at least one. The first
// call starts the workers.
size_t jobs_concurrency();

// Calls body(i) once for every i in [0, count), spread over the workers and
// the calling thread, and returns when every call has finished. Indices are
// claimed one at a time, so uneven jobs balance themselves. When a body
// throws, the remaining indices still run and the first exception is
// rethrown here. A call made from inside a body, or while another thread's
// call is running, runs serially on its own thread.
void jobs_parallel_for(size_t count, const std::function<void(size_t)>& body);

// True on a worker, and on the calling thread while it runs a job body. A
// caller can then pick a serial algorithm instead of a nested batch.
bool jobs_inside();

// printf to stdout for code that may run as a job. Inside a batch the text
// is held per index and printed in index order when the batch ends, so the
// log reads the same as a serial run. Elsewhere it is plain printf.
void jobs_printf(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Holds the current thread's jobs_printf output in out while it lives. For a
// task that runs outside jobs_parallel_for (std::async) and whose caller
// prints the text later, at the place a serial run would.
class JobLogCapture {
 public:
  explicit JobLogCapture(std::string& out);
  ~JobLogCapture();
  JobLogCapture(const JobLogCapture&) = delete;
  JobLogCapture& operator=(const JobLogCapture&) = delete;

 private:
  std::string* previous_;
};

// Stops and joins the workers. Later calls run serially.
void jobs_shutdown();

}  // namespace slrr
