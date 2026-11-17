#include "slrr/ui_time.hpp"
#include <cmath>
#include <cstdio>

namespace slrr {
std::string format_game_time(float seconds, int flags) {
    if (!std::isfinite(seconds)) seconds = 0.f;
    // Show elapsed whole milliseconds: the fourth decimal is discarded,
    // as in race-time displays. Split after quantizing to keep boundaries stable.
    // Clock formats hiding seconds retain their minute boundary.
    double remaining = seconds;
    if (!(flags & 1)) remaining = std::trunc(remaining * 1000.0) / 1000.0;
    int hour = 0, minute = 0;
    if (remaining >= 3600.0) {
      hour = static_cast<int>(remaining / 3600.0);
      remaining -= hour * 3600.0;
    }
    if (remaining >= 60.0) {
      minute = static_cast<int>(remaining / 60.0);
      remaining -= minute * 60.0;
    }
    std::string time;
    char part[64];
    if (!(flags & 2)) {
      if (flags & 32) time = std::to_string(hour == 0 ? 12 : hour > 12 ? hour - 12 : hour);
      else { std::snprintf(part, sizeof(part), "%02d", hour); time = part; }
      time += ':';
    }
    if (!(flags & 4)) {
      std::snprintf(part, sizeof(part), (flags & 2) ? "%d" : "%02d", minute);
      time += part;
    }
    if (!(flags & 1)) {
      if (!(flags & 4)) time += ':';
      if (remaining < 10.0 && !(flags & 4)) time += '0';
      std::snprintf(part, sizeof(part), "%.3f", remaining);
      time += part;
    }
    if (!(flags & 2) && (flags & 32)) time += hour >= 12 ? "pm" : "am";
    return time;
}
}  // namespace slrr
