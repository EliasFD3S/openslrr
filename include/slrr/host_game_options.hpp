#pragma once
namespace slrr {
class Jvm;
void host_game_options_load(Jvm& jvm);
void host_game_options_dirty();
void host_game_options_update(Jvm& jvm, bool force = false);
}
