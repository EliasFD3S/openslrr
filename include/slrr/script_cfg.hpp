#pragma once

#include "slrr/gameref.hpp"
#include "slrr/rpk_index.hpp"

#include <cstdint>

namespace slrr {

// Object scripts (frontend/scripts/*.cfg and the same text stored in an RPK).
// Fills body / trigger / render / name on the instance. Unknown lines are skipped.
void apply_script_cfg(const RpkIndex& rpk, GameRefState* st, uint32_t full_id);

}  // namespace slrr
