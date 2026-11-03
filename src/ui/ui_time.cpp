#include "slrr/ui_time.hpp"
#include <cmath>
#include <cstdio>

namespace slrr {
std::string format_game_time(float seconds, int flags) {
    if (!std::isfinite(seconds)) seconds = 0.f;
    int hour = 0, minute = 0;
    if (seconds >= 3600.f) {
      hour = static_cast<int>(seconds / 3600.f);
      seconds -= hour * 3600.f;
    }
    if (seconds >= 60.f) {
      minute = static_cast<int>(seconds / 60.f);
      seconds -= minute * 60.f;
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
      if (seconds < 10.f && !(flags & 4)) time += '0';
      std::snprintf(part, sizeof(part), "%g", static_cast<double>(seconds));
      time += part;
    }
    if (!(flags & 2) && (flags & 32)) time += hour >= 12 ? "pm" : "am";
    return time;
}
}  // namespace slrr
