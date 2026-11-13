#include "slrr/trigger_volume.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace slrr {
namespace {

std::string trimmed(std::string text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  text.erase(0, first);
  text.erase(text.find_last_not_of(" \t") + 1);
  return text;
}

bool finite_number(const std::string& text, float& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  out = std::strtof(text.c_str(), &end);
  return end && *end == '\0' && std::isfinite(out);
}

}  // namespace

bool trigger_volume_parse(const std::string& params, TriggerVolume& output) {
  std::vector<std::string> fields;
  size_t start = 0;
  for (;;) {
    const size_t comma = params.find(',', start);
    fields.push_back(trimmed(params.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start)));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (fields.size() < 8) return false;
  TriggerVolume volume;
  for (int i = 0; i < 6; ++i)
    if (!finite_number(fields[static_cast<size_t>(i)], volume.pose[i])) return false;
  const auto positive = [&](size_t index, float& out) {
    return index < fields.size() && finite_number(fields[index], out) && out > 0.f;
  };
  if (fields[6] == "sphere" && positive(7, volume.size[0])) {
    volume.shape = TriggerShape::Sphere;
  } else if (fields[6] == "box" && positive(7, volume.size[0]) &&
             positive(8, volume.size[1]) && positive(9, volume.size[2])) {
    volume.shape = TriggerShape::Box;
  } else {
    return false;
  }
  output = volume;
  return true;
}

bool trigger_volume_contains(TriggerShape shape, const float size[3], const float local[3]) {
  if (shape == TriggerShape::Sphere)
    return local[0] * local[0] + local[1] * local[1] + local[2] * local[2] <=
           size[0] * size[0];
  if (shape == TriggerShape::Box)
    return std::fabs(local[0]) <= size[0] && std::fabs(local[1]) <= size[1] &&
           std::fabs(local[2]) <= size[2];
  return false;
}

TriggerEdge trigger_contact_update(TriggerContact& contact, bool inside, bool teleported) {
  const bool silent = !contact.known || teleported || contact.inside == inside;
  contact.known = true;
  contact.inside = inside;
  if (silent) return TriggerEdge::None;
  return inside ? TriggerEdge::Enter : TriggerEdge::Leave;
}

}  // namespace slrr
