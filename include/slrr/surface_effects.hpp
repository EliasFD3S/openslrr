#pragma once
#include <cstdint>

namespace slrr {

class RpkIndex;

// A ground material's skid effects (#454), from the RPK type-14 entries its
// `skid_PS` and `skid_SM` name: `particle <cfg>` or `skidmark <cfg>` plus a
// `texture` id in that pack's slots. Each id is read once and registered.

// The particle_clouds() index of a cloud; 0 means the original's default,
// particles:2 (asphalt smoke). 0xffff when the entry or its cfg is missing.
uint16_t surface_particle_cloud(const RpkIndex &rpk, uint32_t full_id);

// The skid_mark_styles() index of a mark; 0 means particles:0xA (the
// asphalt mark). The untextured default (index 0) when it cannot be read.
uint16_t surface_skid_style(const RpkIndex &rpk, uint32_t full_id);

}  // namespace slrr
