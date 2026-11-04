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
      if (command == "slot") {
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
      } else if (command == "wheel")
        out.wheels.push_back(pose(t));
      else if (command == "steering") {
        out.steering = pose(t);
        out.has_steering = true;
      } else if (command == "stockpart")
        out.stock.push_back(rid(t.at(1)));
      else if (command == "render") {
        PartRender render;
        render.resource = rid(t.at(1));
        if (t.size() == 5)
          for (int i = 0; i < 3; i++)
            render.pose.position[i] = number(t, 2 + i);
        else if (t.size() > 2)
          render.pose = pose(t, 2);
        out.renders.push_back(render);
      } else if (command == "deformable") {
        deformable = rid(t.at(1));
        if (t.size() > 2)
          deformable_pose = pose(t, 2);
      } else if (command == "flags" && !out.renders.empty())
        out.renders.back().flags |= static_cast<uint32_t>(integer(t, 1));
      else if (command == "lod" && !out.renders.empty())
        out.renders.back().lod = true;
      else if (command == "mesh")
        out.mesh = rid(t.at(1));
      else if (command == "texture")
        out.texture = rid(t.at(1));
      else if (command == "category")
        out.category = integer(t, 1);
      else if (command == "body" && t.size() >= 8)
        out.mass += number(t, 7);
      else if (command == "name") {
        for (size_t i = 1; i < t.size(); i++) {
          if (i > 1)
            out.name += ' ';
          out.name += t[i];
        }
      } else
        out.unknown.push_back(raw);
    } catch (const std::exception &e) {
      out.errors.push_back("line " + std::to_string(line) + " (" + command +
                           "): " + e.what());
    }
  }
  if (deformable) {
    out.render = deformable;
    out.render_pose = deformable_pose;
  } else if (!out.renders.empty()) {
    out.render = out.renders[0].resource;
    out.render_pose = out.renders[0].pose;
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
