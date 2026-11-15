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

// Stops and joins the workers. Later calls run serially.
void jobs_shutdown();

}  // namespace slrr
