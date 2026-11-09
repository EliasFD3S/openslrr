#pragma once

namespace slrr {
// Writes a physics diagnostic line to stdout and, when SLRR_PHYSICS_DUMP or
// SLRR_PHYSICS_PROFILE is enabled, to SLRR_PHYSICS_LOG (default
// slrr-physics.log). The file is flushed after every call.
void physics_log(const char *format, ...);
} // namespace slrr
