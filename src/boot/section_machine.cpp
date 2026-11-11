#include "slrr/section_machine.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/diagnostics.hpp"

#include <cstdio>

namespace slrr {
namespace {

constexpr const char* kSelectorOwner = "_host_selector_career_player";

Object* object_field(Object* object, const char* name) {
  if (!object) return nullptr;
  const auto field = object->fields.find(name);
  return field != object->fields.end() && field->second.tag == ValueTag::Obj
             ? field->second.v.o : nullptr;
}

void reset_catalog_return_pose(Jvm& jvm, Object* previous, Object* next) {
  if (!previous || !next) return;
  if (previous->class_name == "java.game.Garage" &&
      next->class_name == "java.game.Catalog") {
    // Stock CMD_CATALOG sets SavePos = 1. Garage.exit then keeps both the
    // live car pose and jack status. Use the stock reset branch's values.
    previous->fields["StartPos"] =
        Value::make_obj(jvm.new_object("java.lang.Vector3"));
    previous->fields["StartYpr"] =
        Value::make_obj(jvm.new_object("java.lang.Ypr"));
    previous->fields["JackStatus"] = Value::make_int(0);
  } else if (previous->class_name == "java.game.Catalog" &&
             next->class_name == "java.game.Garage") {
    auto* car = object_field(object_field(next, "player"), "car");
    auto* pos = object_field(next, "StartPos");
    auto* ypr = object_field(next, "StartYpr");
    if (!car || !pos || !ypr) return;
    // Returning from Catalog can reuse the same live chassis. Apply the
    // parking pose even when the script skips placing that chassis again.
    jvm.invoke("java.util.resource.GameRef", "setMatrix",
               "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
               {Value::make_obj(car), Value::make_obj(pos), Value::make_obj(ypr)},
               false);
  }
}

void begin_selector_preview(Jvm& jvm, Object* selector) {
  if (!selector || selector->class_name != "java.game.CarSelect") return;
  const auto mode = selector->fields.find("Mode");
  auto* player = object_field(selector, "player");
  if (!player || object_field(selector, kSelectorOwner) ||
      mode == selector->fields.end() || mode->second.tag != ValueTag::Int ||
      mode->second.v.i != 0)
    return;
  const Value career = jvm.get_static("java.game.GameLogic", "player");
  if (career.tag != ValueTag::Obj || career.v.o != player) return;
  // MWM's standalone selector uses player.car as its preview slot. Keep the
  // career player available to the menu and its automatic saves.
  auto* preview_player = jvm.new_object("java.game.Player");
  selector->fields[kSelectorOwner] = Value::make_obj(player);
  selector->fields["player"] = Value::make_obj(preview_player);
  if (auto* take = object_field(selector, "TakeButton"))
    jvm.invoke_by_name(take->class_name, "disable", {Value::make_obj(take)}, false);
}

bool finish_selector_preview(Jvm& jvm, Object* selector, bool take_car) {
  auto* player = object_field(selector, kSelectorOwner);
  if (!player) return true;
  auto* preview = object_field(object_field(selector, "player"), "car");
  if (take_car && !preview) return false;
  auto* discarded = take_car ? object_field(player, "car") : preview;
  if (take_car) player->fields["car"] = Value::make_obj(preview);
  selector->fields["player"] = Value::make_obj(player);
  selector->fields.erase(kSelectorOwner);
  if (discarded && (!take_car || discarded != preview))
    jvm.invoke_by_name(discarded->class_name, "destroy",
                       {Value::make_obj(discarded)}, false);
  return true;
}

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
  auto* old = actual_;
  actual_ = next;
  actual_name_ = next ? next->class_name : "(null)";
  if (diagnostics::enabled()) {
    diagnostics::set_section(actual_name_);
    diagnostics::Event event;
    event.category = diagnostics::Category::Section;
    event.name = "section.transition";
    event.object = next;
    event.class_name = actual_name_;
    event.detail = (old ? old->class_name : "(null)") + " -> " + actual_name_;
    event.result = "adopted";
    diagnostics::trace(std::move(event));
  }
  if (jvm_) {
    reset_catalog_return_pose(*jvm_, old, next);
    finish_selector_preview(*jvm_, old, false);
    begin_selector_preview(*jvm_, next);
  }
  std::printf("[section] adopt %s\n", actual_name_.c_str());
}

void SectionMachine::dispatch_osd_command(Object* handler, int32_t command) {
  if (!jvm_ || !handler || handler->class_name.empty()) return;
  if (handler == actual_ && object_field(handler, kSelectorOwner)) {
    const Value take = jvm_->get_static(handler->class_name, "CMD_OK");
    if (take.tag == ValueTag::Int && take.v.i == command &&
        !finish_selector_preview(*jvm_, handler, true))
      return;
  }
  if (host_commit_video_dialog(*jvm_, handler, command)) return;
  jvm_->invoke(handler->class_name, "osdCommand", "(I)V",
               {Value::make_obj(handler), Value::make_int(command)}, false);
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
  if (diagnostics::enabled()) {
    diagnostics::set_section(actual_name_);
    diagnostics::Event event;
    event.category = diagnostics::Category::Section;
    event.name = "section.transition";
    event.object = next;
    event.class_name = actual_name_;
    event.detail = (old ? old->class_name : "(null)") + " -> " + actual_name_;
    event.result = "changed";
    diagnostics::trace(std::move(event));
  }
  jvm_->set_static("java.game.GameLogic", "actualState",
                   Value::make_obj(actual_));
  std::printf("[section] actual=%s\n", actual_name_.c_str());
  if (actual_) {
    std::printf("[section] enter %s\n", actual_->class_name.c_str());
    call_lifecycle(jvm_, actual_, "enter", old);
  }
  if (jvm_) reset_catalog_return_pose(*jvm_, old, next);
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
