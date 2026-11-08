#pragma once

#include "slrr/jvm.hpp"
#include "slrr/types.hpp"

#include <string>

namespace slrr {

// Host-side GameLogic.changeActiveSection: exit(old) → actual=new → enter(new).
class SectionMachine {
 public:
  explicit SectionMachine(Jvm* jvm);

  Object* actual() const { return actual_; }
  Object* garage() const { return garage_; }
  const std::string& actual_name() const { return actual_name_; }

  void set_garage(Object* g);
  // Seed GameLogic.player (+ controller) before OptionsDialog / MainMenuDialog.
  Object* seed_player(const std::string& player_name = "Player");
  // Change section; invokes exit/enter by name when present.
  void change_active_section(Object* next);
  // Java changeActiveSection already called exit/enter. Keep the host
  // pointer on GameLogic.actualState without entering a second time.
  void adopt_actual(Object* next);
  void dispatch_osd_command(Object* handler, int32_t command);
  // --auto-new shortcut: MainMenu → Garage, skip modal dialogs.
  bool auto_new_career(const std::string& player_name);

 private:
  Jvm* jvm_ = nullptr;
  Object* actual_ = nullptr;
  Object* garage_ = nullptr;
  std::string actual_name_ = "(none)";
};

}  // namespace slrr
