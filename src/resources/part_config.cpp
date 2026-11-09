#include "slrr/part_config.hpp"
#include "slrr/rpk_index.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace slrr {
namespace {
PartMatrix identity() {
  PartMatrix m{};
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}
std::vector<std::string> words(std::string line) {
  size_t cut = line.size();
  for (const char *marker : {"#", ";", "//"})
    cut = std::min(cut, line.find(marker));
  line.resize(cut);
  std::istringstream input(line);
  std::vector<std::string> out;
  for (std::string word; input >> word;)
    out.push_back(word);
  return out;
}
float number(const std::vector<std::string> &t, size_t index) {
  if (index >= t.size())
    throw std::runtime_error("missing number");
  size_t used = 0;
  float value = std::stof(t[index], &used);
  if (used != t[index].size() || !std::isfinite(value))
    throw std::runtime_error("invalid finite number");
  return value;
}
int integer(const std::vector<std::string> &t, size_t index) {
  float value = number(t, index);
  if (value < 0 || value > 16777215 || std::floor(value) != value)
    throw std::runtime_error("invalid slot/category integer");
  return static_cast<int>(value);
}
PartPose pose(const std::vector<std::string> &t, size_t offset = 1) {
  PartPose result;
  for (size_t i = 0; i < 3; i++) {
    result.position[i] = number(t, offset + i);
    result.rotation[i] = number(t, offset + 3 + i);
  }
  return result;
}
} // namespace

PartMatrix part_matrix_multiply(const PartMatrix &a, const PartMatrix &b) {
  PartMatrix out{};
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 4; k++)
        out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
  return out;
}
PartMatrix part_pose_matrix(const PartPose &p) {
  auto y = identity(), x = identity(), z = identity();
  float cy = std::cos(p.rotation[0]), sy = std::sin(p.rotation[0]);
  float cp = std::cos(p.rotation[1]), sp = std::sin(p.rotation[1]);
  float cr = std::cos(p.rotation[2]), sr = std::sin(p.rotation[2]);
  y[0] = cy;
  y[2] = -sy;
  y[8] = sy;
  y[10] = cy;
  x[5] = cp;
  x[6] = sp;
  x[9] = -sp;
  x[10] = cp;
  z[0] = cr;
  z[1] = sr;
  z[4] = -sr;
  z[5] = cr;
  auto out = part_matrix_multiply(part_matrix_multiply(y, x), z);
  for (int i = 0; i < 3; i++)
    out[12 + i] = p.position[i];
  return out;
}
PartMatrix part_matrix_inverse_rigid(const PartMatrix &m) {
  auto out = identity();
  for (int c = 0; c < 3; c++)
    for (int r = 0; r < 3; r++)
      out[c * 4 + r] = m[r * 4 + c];
  for (int r = 0; r < 3; r++)
    out[12 + r] = -(out[r] * m[12] + out[4 + r] * m[13] + out[8 + r] * m[14]);
  return out;
}
bool part_parked_spring_length(float rest, float minimum, float maximum,
                               float force, float mass_on_wheel,
                               float &length) {
  if (!(force > 0.f && rest > 0.f) || !std::isfinite(mass_on_wheel))
    return false;
  const float low = minimum > 0.f ? minimum : 0.f;
  const float high = maximum > 0.f ? maximum : rest;
  const float sag = mass_on_wheel * 9.81f / force;
  length = std::clamp(rest - sag, low, high);
  return true;
}

bool part_slot_matrix(const PartConfig &config, const PartSlot &slot,
                      PartMatrix &matrix) {
  PartMatrix frame = identity();
  if (slot.type == 11) {
    if (!config.has_steering)
      return false;
    frame = part_pose_matrix(config.steering);
  } else if ((slot.type >= 101 && slot.type <= 199) ||
             (slot.type >= 201 && slot.type <= 204) ||
             (slot.type >= 301 && slot.type <= 304)) {
    int wheel = slot.type % 100 - 1;
    if (wheel < 0 || static_cast<size_t>(wheel) >= config.wheels.size())
      return false;
    frame = part_pose_matrix(config.wheels[wheel]);
  } else if (slot.type >= 500) {
    // Einvagen hangs the front spring on slottype 501, the upper-arm
    // locator whose slot id is 501. The wheel-body codes stop at 304.
    const PartSlot *locator = nullptr;
    for (const auto &other : config.slots)
      if (other.id == slot.type && other.type < 500) {
        locator = &other;
        break;
      }
    if (!locator || !part_slot_matrix(config, *locator, frame))
      return false;
  } else if (slot.type != 0)
    return false;
  matrix = part_matrix_multiply(frame, part_pose_matrix(slot.pose));
  return true;
}

PartConfig parse_part_config(const std::string &source,
                             const RpkIndex *resources, int owner_pack) {
  PartConfig out;
  std::istringstream input(source);
  std::string raw;
  size_t line = 0;
  uint32_t deformable = 0;
  PartPose deformable_pose;
  bool have_spring = false;
  // The wheel whose `render` lines are still open. Slot and stock lines
  // close it, so a body render is never tagged as a wheel.
  int open_wheel = -1;
  float spring[3] = {0.29f, 0.1f, 0.05f};
  float wheel_radius = 0.f;
  float wheel_mass = 0.f;
  auto rid = [&](const std::string &text) {
    std::string word = text;
    if (!word.empty() && (word.back() == 'r' || word.back() == 'R'))
      word.pop_back();
    size_t used = 0;
    unsigned long long value = std::stoull(word, &used, 0);
    if (used != word.size() || value > UINT32_MAX)
      throw std::runtime_error("invalid resource reference");
    return resources ? resources->resolve_embedded_id(
                           owner_pack, static_cast<uint32_t>(value))
                     : static_cast<uint32_t>(value);
  };
  while (std::getline(input, raw)) {
    ++line;
    auto t = words(raw);
    if (t.empty())
      continue;
    std::string command = t[0];
    std::transform(
        command.begin(), command.end(), command.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    try {
      if (command == "eof")
        break;
      if (command.starts_with("osd_")) {
        constexpr const char *names[] = {"osd_gauge", "osd_rpmpin", "osd_spdpin",
            "osd_rpm", "osd_speed", "osd_gear", "osd_mileage", "osd_hbrake",
            "osd_damage", "osd_nos", "osd_turbo"};
        const auto field = std::find(std::begin(names), std::end(names), command);
        if (field == std::end(names)) continue;
        const size_t slot = static_cast<size_t>(field - std::begin(names));
        HudElement item;
        const auto raw_id = std::stoull(t.at(1), nullptr, 0);
        if (raw_id > UINT32_MAX) continue;
        item.resource = raw_id == 0 || raw_id == UINT32_MAX ? 0 : rid(t[1]);
        for (size_t i = 0; i < item.position.size(); ++i)
          item.position[i] = std::stof(t.at(i + 2));
        if (slot == size_t(HudSlot::RpmPin) || slot == size_t(HudSlot::SpeedPin)) {
          for (size_t i = 0; i < item.needle.size(); ++i)
            item.needle[i] = std::stof(t.at(i + 5));
        } else if (slot >= size_t(HudSlot::Rpm) && slot <= size_t(HudSlot::Mileage)) {
          if (t.size() > 5) item.color = static_cast<uint32_t>(std::stoull(t[5], nullptr, 0));
          if (t.size() > 6) item.format = t[6];
        }
        if (std::all_of(item.position.begin(), item.position.end(), [](float n) { return std::isfinite(n); }) &&
            std::all_of(item.needle.begin(), item.needle.end(), [](float n) { return std::isfinite(n); })) {
          item.defined = true;
          out.hud[slot] = std::move(item);
        }
      } else if (command == "slot") {
        open_wheel = -1;
        PartSlot slot;
        slot.pose = pose(t);
        // PE sscanf "%f %f %f %f %f %f %d": a missing id stays 0.
        // Duhen SunStrip 1.8 and 2.2 omit it on the FR shock line. Rejecting
        // the whole cfg dropped those two cars, and every later catalog
        // click loaded the chassis two slots earlier.
        if (t.size() > 7)
          slot.id = integer(t, 7);
        if (std::any_of(out.slots.begin(), out.slots.end(),
                        [&](const auto &prior) { return prior.id == slot.id; }))
          throw std::runtime_error("duplicate slot ID");
        out.slots.push_back(slot);
      } else if (command == "attach" || command == "compatible") {
        if (out.slots.empty())
          throw std::runtime_error("attachment before slot");
        PartSlotLink link{rid(t.at(1)), integer(t, 2)};
        (command == "attach" ? out.slots.back().attach
                             : out.slots.back().compatible)
            .push_back(link);
      } else if (command == "slottype") {
        if (out.slots.empty())
          throw std::runtime_error("slottype before slot");
        out.slots.back().type = integer(t, 1);
      } else if (!out.slots.empty() &&
                 ((command == "flap" && t.size() >= 12) ||
                  (t.size() == 11 &&
                 (std::isdigit(static_cast<unsigned char>(command[0])) ||
                  command[0] == '-' || command[0] == '+' ||
                  command[0] == '.')))) {
        PartSlot &slot = out.slots.back();
        // Stock CFGs prefix the hinge parameters with `flap`. Older
        // files may contain the same eleven numbers without the keyword.
        const size_t first = command == "flap" ? 1 : 0;
        for (int i = 0; i < 3; ++i) {
          slot.flap_pivot[i] = number(t, first + i);
          slot.flap_axis[i] = number(t, first + 3 + i);
          slot.flap_dir[i] = number(t, first + 6 + i);
        }
        slot.flap_lo = number(t, first + 9);
        slot.flap_hi = number(t, first + 10);
        slot.has_flap = true;
      } else if (command == "type") {
        have_spring = false;
        wheel_radius = 0.f;
        wheel_mass = t.size() >= 2 ? number(t, 1) : 0.f;
        if (wheel_mass < 0.f)
          throw std::runtime_error("wheel mass must be non-negative");
        if (t.size() >= 4 && (t[2] == "sphere" || t[2] == "Sphere")) {
          wheel_radius = number(t, 3);
          if (wheel_radius <= 0.f)
            throw std::runtime_error("wheel radius must be positive");
        }
      } else if (command == "spring") {
        for (int i = 0; i < 3; ++i)
          spring[i] = number(t, static_cast<size_t>(i + 1));
        if (spring[0] <= 0.f || spring[1] <= 0.f || spring[2] <= 0.f)
          throw std::runtime_error("spring components must be positive");
        have_spring = true;
      } else if (command == "wheel") {
        out.wheels.push_back(pose(t));
        out.wheel_steering.push_back(t.size() > 7 ? number(t, 7) : 0.f);
        PartWheelSpring spec;
        spec.radius = wheel_radius;
        spec.mass = wheel_mass;
        if (have_spring) {
          spec.base = spring[0];
          spec.travel = spring[1];
          spec.damping = spring[2];
          spec.authored = true;
        }
        out.wheel_springs.push_back(spec);
        open_wheel = static_cast<int>(out.wheels.size()) - 1;
      } else if (command == "maxsteer") {
        open_wheel = -1;
        out.max_steer = number(t, 1);
        out.ackermann_length = t.size() > 2 ? number(t, 2) : 0.f;
      } else if (command == "brakepower" && t.size() > 2) {
        open_wheel = -1;
        out.brake_power = number(t, 1);
        out.handbrake_power = number(t, 2);
      } else if (command == "enginepower" && t.size() > 6) {
        open_wheel = -1;
        out.brake_inertia = number(t, 6);
      } else if (command == "steering") {
        open_wheel = -1;
        out.steering = pose(t);
        out.has_steering = true;
      } else if (command == "stockpart") {
        open_wheel = -1;
        out.stock.push_back(rid(t.at(1)));
      } else if (command == "render") {
        PartRender render;
        render.resource = rid(t.at(1));
        render.wheel = open_wheel;
        if (t.size() == 5)
          for (int i = 0; i < 3; i++)
            render.pose.position[i] = number(t, 2 + i);
        else if (t.size() > 2)
          render.pose = pose(t, 2);
        out.renders.push_back(render);
      } else if (command == "colors" && !out.renders.empty()) {
        for (size_t i = 2; i < t.size(); ++i)
          out.renders.back().colors.push_back(
              static_cast<uint32_t>(std::stoul(t[i], nullptr, 0)) & 0xffffffu);
      } else if (command == "deformable") {
        open_wheel = -1;
        deformable = rid(t.at(1));
        if (t.size() > 2)
          deformable_pose = pose(t, 2);
      } else if (command == "flags" && !out.renders.empty())
        out.renders.back().flags |= static_cast<uint32_t>(integer(t, 1));
      else if (command == "lod" && !out.renders.empty())
        out.renders.back().lod_level = integer(t, 1);
      else if (command == "linked" && !out.renders.empty()) {
        PartLink link;
        link.first = integer(t, 1);
        if (t.size() > 2)
          link.second = integer(t, 2);
        if (t.size() > 3) {
          link.parent_slot = integer(t, 3);
          link.filter = true;
        }
        out.renders.back().links.push_back(link);
      }       else if (command == "mesh") {
        open_wheel = -1;
        out.mesh = rid(t.at(1));
      } else if (command == "texture") {
        open_wheel = -1;
        out.texture = rid(t.at(1));
      } else if (command == "category") {
        open_wheel = -1;
        out.category = integer(t, 1);
      } else if (command == "body" && t.size() >= 8) {
        open_wheel = -1;
        PartBody body;
        body.pose = pose(t);
        body.mass = number(t, 7);
        if (body.mass < 0.f)
          throw std::runtime_error("body mass must be non-negative");
        out.bodies.push_back(body);
        out.mass += body.mass;
      } else if (command == "name") {
        open_wheel = -1;
        for (size_t i = 1; i < t.size(); i++) {
          if (i > 1)
            out.name += ' ';
          out.name += t[i];
        }
      } else
        out.unknown.push_back(raw);
    } catch (const std::exception &e) {
      if (command.starts_with("osd_")) continue;
      out.errors.push_back("line " + std::to_string(line) + " (" + command +
                           "): " + e.what());
    }
  }
  if (deformable) {
    out.render = deformable;
    out.render_pose = deformable_pose;
  } else if (!out.renders.empty()) {
    // The first render is the lo-res box. The catalog camera sits inside the
    // closest band, which is the last level (the deck and the bores).
    const PartRender *chosen = &out.renders[0];
    for (const auto &r : out.renders)
      if (!r.flags && r.wheel < 0 &&
          (chosen->flags || chosen->wheel >= 0 ||
           r.lod_level >= chosen->lod_level))
        chosen = &r;
    out.render = chosen->resource;
    out.render_pose = chosen->pose;
  }
  out.valid = out.errors.empty() && (!out.slots.empty() || out.render != 0);
  return out;
}

PartConfig load_part_config(const RpkIndex &resources, uint32_t resource) {
  PartConfig missing;
  missing.resource = resource;
  const std::string native = resources.payload_prop(resource, "native");
  std::istringstream properties(native);
  std::string kind, path;
  properties >> kind;
  std::getline(properties, path);
  size_t begin = path.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return missing;
  path = path.substr(begin);
  while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back())))
    path.pop_back();
  if (kind != "car" && kind != "part")
    return missing;
  std::replace(path.begin(), path.end(), '\\', '/');
  std::ifstream input(std::filesystem::path(resources.game_root()) / path,
                      std::ios::binary);
  if (!input) {
    missing.errors.push_back("missing CFG: " + path);
    return missing;
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  auto out = parse_part_config(bytes.str(), &resources,
                               RpkIndex::pack_from_full(resource));
  out.resource = resource;
  out.source = path;
  out.native_kind = kind;
  return out;
}
} // namespace slrr
