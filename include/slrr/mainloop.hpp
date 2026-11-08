#pragma once

#include "slrr/jvm.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/section_machine.hpp"

namespace slrr {

// Soft PE Engine_MainLoop @ 0x00428960 (size 0x651) — lean host frame pulse.
// Stock order (Runtime System_natives.cpp system_mainloop_sfx_listener_update):
//   Input_tick → SimulateFrame (timers/phys) → Jvm_PumpFrame →
//   animate/drain → Sfx → RE pumps → AltPresent|Present → AsyncLoad →
//   PollWindowQuit → EndFrame.
// Host implements the script-visible slice; phys/async/sfx remain stubs.

struct MainLoopFrameResult {
  int32_t timers_fired = 0;
  bool quit = false;
};

// One Soft frame. `dt` = wall seconds since previous pulse (clamped like PE).
MainLoopFrameResult mainloop_frame(Jvm& jvm, IRenderBackend& render,
                                   SectionMachine& sections, double dt);

// Garage SCX vs menu OSD overlay from actual section.
void mainloop_sync_section_render(IRenderBackend& render,
                                  SectionMachine& sections);

// Follow GameLogic.actualState after the Java changeActiveSection TREE.
void mainloop_adopt_section(Jvm& jvm, SectionMachine& sections);

// Mechanic.run is the inventory spinner. Thread.start does not enter it,
// so the garage "Welcome!" placeholder never became the slot line.
void mainloop_note_thread(Jvm& jvm, Object* thread, bool started);

}  // namespace slrr
