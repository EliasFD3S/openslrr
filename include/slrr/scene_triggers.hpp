#pragma once

namespace slrr {

class Jvm;

// GameRef.RID_TRIGGER volumes (java.game.Trigger) report EVENT_TRIGGER_ON
// (0x20) when a top-level car of the active world enters them and
// EVENT_TRIGGER_OFF (0x40) when it leaves. The source is the trigger GameRef
// and the String param starts with the car's id(), which the stock handlers
// read with param.token(0). A car seen inside a volume for the first time
// raises nothing, nor does a car that moves more than 25 m in one frame (a
// setMatrix teleport); a shorter setMatrix move counts like driving.
void scene_triggers_tick(Jvm& jvm);
void scene_triggers_reset();

}  // namespace slrr
