#pragma once
#include <string>
namespace slrr {
// String.java timeToString flags: NOSECONDS=1, NOHOURS=2, NOMINUTES=4, US=32.
std::string format_game_time(float seconds, int flags);
}
