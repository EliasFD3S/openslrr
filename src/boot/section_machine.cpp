#include "slrr/section_machine.hpp"

#include <cstdio>

namespace slrr {
namespace {

void call_lifecycle(Jvm* jvm, Object* target, const char* method, Object* other) {
  if (!jvm || !target) return;
  const std::string sig = "(Ljava.game.GameState;)V";
  if (jvm->natives().has(target->class_name, method, sig) ||
      jvm->find_class(target->class_name)) {
    jvm->invoke(target->class_name, method, sig,
                 {Value::make_obj(target), Value::make_obj(other)}, false);
  }
}

Object* ensure_player(Jvm* jvm, const std::string& player_name) {
  Object* player = nullptr;
  Value pv = jvm->get_static("java.game.GameLogic", "player");
  if (pv.tag == ValueTag::Obj) player = pv.v.o;
  if (!player) {
    player = jvm->new_object("java.game.Player");
    jvm->set_static("java.game.GameLogic", "player", Value::make_obj(player));
  }
  player->fields["name"] = Value::make_obj(jvm->new_string(player_name));
  // Racer.java defaults — needed for garage HUD (money / prestige strings).
  if (!player->fields.count("money") ||
      player->fields["money"].tag != ValueTag::Int)
    player->fields["money"] = Value::make_int(20000);  // INITIAL_PLAYER_MONEY
  if (!player->fields.count("prestige") ||
      player->fields["prestige"].tag != ValueTag::Float)
    player->fields["prestige"] = Value::make_float(0.3f);
  if (!player->fields.count("club") ||
      player->fields["club"].tag != ValueTag::Int)
    player->fields["club"] = Value::make_int(0);
  if (!player->fields.count("controller") ||
      player->fields["controller"].tag != ValueTag::Obj ||
      !player->fields["controller"].v.o) {
    Object* ctl = jvm->new_object("java.io.Controller");
    player->fields["controller"] = Value::make_obj(ctl);
  }
  return player;
}

}  // namespace

SectionMachine::SectionMachine(Jvm* jvm) : jvm_(jvm) {}

void SectionMachine::set_garage(Object* g) {
  garage_ = g;
  if (jvm_ && g)
    jvm_->set_static("java.game.GameLogic", "garage", Value::make_obj(g));
}

Object* SectionMachine::seed_player(const std::string& player_name) {
  return ensure_player(jvm_, player_name);
}

void SectionMachine::adopt_actual(Object* next) {
  if (next == actual_) return;
  actual_ = next;
  actual_name_ = next ? next->class_name : "(null)";
  std::printf("[section] adopt %s\n", actual_name_.c_str());
}

void SectionMachine::change_active_section(Object* next) {
  Object* old = actual_;
  if (old) {
    std::printf("[section] exit %s\n", old->class_name.c_str());
    call_lifecycle(jvm_, old, "exit", next);
    // Drop pending timers (tree-smoke Splash.addTimer must not fire later).
    if (jvm_)
      jvm_->invoke(old->class_name, "removeAllTimers", "()V",
                   {Value::make_obj(old)}, false);
  }
  actual_ = next;
  actual_name_ = next ? next->class_name : "(null)";
  jvm_->set_static("java.game.GameLogic", "actualState",
                   Value::make_obj(actual_));
  std::printf("[section] actual=%s\n", actual_name_.c_str());
  if (actual_) {
    std::printf("[section] enter %s\n", actual_->class_name.c_str());
    call_lifecycle(jvm_, actual_, "enter", old);
  }
}

bool SectionMachine::auto_new_career(const std::string& player_name) {
  // Stock path: MainMenu → MainMenuDialog.osdCommand(CMD_NEW=50) → Garage.
  constexpr int32_t kCmdNew = 50;
  std::printf("[section] auto-new via osdCommand(CMD_NEW=%d) player=%s\n",
              kCmdNew, player_name.c_str());

  if (!garage_) {
    garage_ = jvm_->new_object("java.game.Garage");
    jvm_->invoke("java.game.Garage", "<init>", "()V",
                 {Value::make_obj(garage_)}, false);
    set_garage(garage_);
  }

  Object* player = ensure_player(jvm_, player_name);
  Object* menu = jvm_->new_object("java.game.MainMenu");
  change_active_section(menu);

  Object* mmd = nullptr;
  auto it = menu->fields.find("mmd");
  if (it != menu->fields.end() && it->second.tag == ValueTag::Obj)
    mmd = it->second.v.o;
  if (!mmd) {
    mmd = jvm_->new_object("java.game.MainMenuDialog");
    menu->fields["mmd"] = Value::make_obj(mmd);
  }
  mmd->fields["player"] = Value::make_obj(player);
  // Preferred name for StringRequesterDialog.display host leaf.
  jvm_->set_static("java.game.GameLogic", "_autoNewPlayerName",
                   Value::make_obj(jvm_->new_string(player_name)));

  jvm_->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V",
               {Value::make_obj(mmd), Value::make_int(kCmdNew)}, false);

  const bool ok =
      actual_ && actual_->class_name.find("Garage") != std::string::npos;
  Value cip = jvm_->get_static("java.game.GameLogic", "carrerInProgress");
  std::printf("[section] CMD_NEW→Garage section=%s career=%d ok=%d\n",
              actual_name_.c_str(),
              cip.tag == ValueTag::Int ? cip.v.i : -1, ok ? 1 : 0);
  return ok;
}

}  // namespace slrr
