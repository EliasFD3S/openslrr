#include "slrr/skeleton.hpp"
#include "slrr/format/bon.hpp"
#include "slrr/format/skl.hpp"

#include "slrr/gameref.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scx_mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace slrr {
namespace {

struct BoneOverride {
  bool set = false;
  float local[16]{};
};

struct Character {
  int32_t type_id = 0;
  bool tried = false;
  std::shared_ptr<const Skeleton> bind;
  std::shared_ptr<const Skeleton> lengths;
  std::shared_ptr<const Clip> clip;
  void* animation = nullptr;
  float speed = 1.f;
  float fade = 0.f;
  float fade_age = 0.f;
  float time = 0.f;
  bool playing = false;
  bool loop = false;
  float last_now = -1.f;
  bool fresh = false;
  float fresh_now = 0.f;
  bool want_skin = false;
  float blend = 1.f;
  std::vector<BoneOverride> overrides;
  std::vector<float> worlds;
  uint32_t pose_serial = 0;
  std::shared_ptr<ScxGeometry> posed;
  const ScxGeometry* posed_from = nullptr;
  uint32_t posed_serial = 0;
};

struct BoneLink {
  void* parent = nullptr;
  int bone = 0;
};

std::unordered_map<void*, Character> g_characters;
std::unordered_map<void*, BoneLink> g_links;
std::unordered_map<void*, void*> g_animations;
std::unordered_map<std::string, std::shared_ptr<const Skeleton>> g_skeletons;
std::unordered_map<std::string, std::shared_ptr<const Clip>> g_clips;

std::string cache_key(std::string path) {
  for (char& ch : path) {
    if (ch == '/') ch = '\\';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return path;
}

std::filesystem::path disk_path(const RpkIndex& packs, const std::string& relative) {
  std::filesystem::path path(relative);
  if (path.is_absolute()) return path;
  return std::filesystem::path(packs.game_root()) / path;
}

bool read_bytes(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const auto size = in.tellg();
  if (size < 0) return false;
  in.seekg(0);
  out->resize(static_cast<size_t>(size));
  if (out->empty()) return true;
  in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  return static_cast<bool>(in);
}

std::shared_ptr<const Skeleton> load_skeleton(const RpkIndex& packs, const std::string& relative) {
  if (relative.empty()) return nullptr;
  const std::string key = cache_key(relative);
  if (const auto it = g_skeletons.find(key); it != g_skeletons.end()) return it->second;
  std::vector<uint8_t> bytes;
  std::shared_ptr<const Skeleton> parsed;
  if (read_bytes(disk_path(packs, relative), &bytes)) {
    Skeleton skeleton;
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const bool lengths = relative.size() >= 4 &&
                         (relative[relative.size() - 3] == 'b' || relative[relative.size() - 3] == 'B') &&
                         (relative[relative.size() - 2] == 'o' || relative[relative.size() - 2] == 'O') &&
                         (relative[relative.size() - 1] == 'n' || relative[relative.size() - 1] == 'N') &&
                         relative[relative.size() - 4] == '.';
    const bool ok = lengths ? bon_read(text, &skeleton, nullptr) : skl_read(text, &skeleton, nullptr);
    if (ok) {
      skeleton_prepare_bind(skeleton);
      parsed = std::make_shared<const Skeleton>(std::move(skeleton));
    }
  }
  g_skeletons.emplace(key, parsed);
  return parsed;
}

std::shared_ptr<const Clip> load_clip(const RpkIndex& packs, const std::string& relative) {
  if (relative.empty()) return nullptr;
  const std::string key = cache_key(relative);
  if (const auto it = g_clips.find(key); it != g_clips.end()) return it->second;
  std::vector<uint8_t> bytes;
  std::shared_ptr<const Clip> parsed;
  if (read_bytes(disk_path(packs, relative), &bytes) && !bytes.empty()) {
    Clip clip;
    if (clip_parse(bytes.data(), bytes.size(), &clip, nullptr))
      parsed = std::make_shared<const Clip>(std::move(clip));
  }
  g_clips.emplace(key, parsed);
  return parsed;
}

std::string sibling_bon(std::string path) {
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    path.replace(dot, std::string::npos, ".bon");
  else
    path += ".bon";
  return path;
}

bool has_extension(const std::string& path, const char* extension) {
  const size_t length = std::strlen(extension);
  if (path.size() < length) return false;
  for (size_t i = 0; i < length; ++i)
    if (std::tolower(static_cast<unsigned char>(path[path.size() - length + i])) !=
        std::tolower(static_cast<unsigned char>(extension[i])))
      return false;
  return true;
}

// A render descriptor names its SKL with `skeleton`. The stock spring and
// shock absorber renders (and mods derived from them, such as MW_Mod's
// Pneumatic_SA) declare none: their skinned SCX sits next to a BON of the
// same name, the two-bone rig the original `add_linked` binds as bone00 and
// bone01. A mesh without skin weights ignores a skeleton, so the fallback is
// harmless for any other render that happens to have a sibling BON.
std::string render_skeleton(const RpkIndex& packs, int32_t type_id) {
  const auto full = static_cast<uint32_t>(type_id);
  std::string declared = packs.payload_prop(full, "skeleton");
  if (!declared.empty()) return declared;
  const auto model = packs.model_for_resource(RpkIndex::pack_from_full(full), full & 0xffffu);
  if (!model.ok || !has_extension(model.source, ".scx")) return {};
  std::string bon = sibling_bon(model.source);
  std::error_code error;
  return std::filesystem::is_regular_file(disk_path(packs, bon), error) ? bon : std::string{};
}

void identity(float* m) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

void multiply(float* out, const float* a, const float* b) {
  float result[16]{};
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
  std::memcpy(out, result, sizeof(result));
}

void ypr_matrix(float* m, const float position[3], const float ypr[3]) {
  float yaw[16], pitch[16], roll[16];
  identity(yaw);
  identity(pitch);
  identity(roll);
  const float cy = std::cos(ypr[0]), sy = std::sin(ypr[0]);
  const float cp = std::cos(ypr[1]), sp = std::sin(ypr[1]);
  const float cr = std::cos(ypr[2]), sr = std::sin(ypr[2]);
  yaw[0] = cy;
  yaw[2] = -sy;
  yaw[8] = sy;
  yaw[10] = cy;
  pitch[5] = cp;
  pitch[6] = sp;
  pitch[9] = -sp;
  pitch[10] = cp;
  roll[0] = cr;
  roll[1] = sr;
  roll[4] = -sr;
  roll[5] = cr;
  multiply(m, yaw, pitch);
  multiply(m, m, roll);
  m[12] = position[0];
  m[13] = position[1];
  m[14] = position[2];
}

void quat_from_matrix(const float* m, float* q) {
  const float trace = m[0] + m[5] + m[10];
  if (trace > 0.f) {
    const float s = std::sqrt(trace + 1.f) * 2.f;
    q[3] = 0.25f * s;
    q[0] = (m[6] - m[9]) / s;
    q[1] = (m[8] - m[2]) / s;
    q[2] = (m[1] - m[4]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const float s = std::sqrt(1.f + m[0] - m[5] - m[10]) * 2.f;
    q[3] = (m[6] - m[9]) / s;
    q[0] = 0.25f * s;
    q[1] = (m[1] + m[4]) / s;
    q[2] = (m[8] + m[2]) / s;
  } else if (m[5] > m[10]) {
    const float s = std::sqrt(1.f + m[5] - m[0] - m[10]) * 2.f;
    q[3] = (m[8] - m[2]) / s;
    q[0] = (m[1] + m[4]) / s;
    q[1] = 0.25f * s;
    q[2] = (m[6] + m[9]) / s;
  } else {
    const float s = std::sqrt(1.f + m[10] - m[0] - m[5]) * 2.f;
    q[3] = (m[1] - m[4]) / s;
    q[0] = (m[8] + m[2]) / s;
    q[1] = (m[6] + m[9]) / s;
    q[2] = 0.25f * s;
  }
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (length < 1e-8f) {
    q[0] = q[1] = q[2] = 0.f;
    q[3] = 1.f;
    return;
  }
  for (int i = 0; i < 4; ++i) q[i] /= length;
}

void quat_to_matrix(float* m, const float* q, const float* translation) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  identity(m);
  m[0] = 1.f - 2.f * (yy + zz);
  m[1] = 2.f * (xy + wz);
  m[2] = 2.f * (xz - wy);
  m[4] = 2.f * (xy - wz);
  m[5] = 1.f - 2.f * (xx + zz);
  m[6] = 2.f * (yz + wx);
  m[8] = 2.f * (xz + wy);
  m[9] = 2.f * (yz - wx);
  m[10] = 1.f - 2.f * (xx + yy);
  m[12] = translation[0];
  m[13] = translation[1];
  m[14] = translation[2];
}

void blend_local(const float* from, const float* to, float weight, float* out) {
  float qa[4], qb[4], mixed[4];
  quat_from_matrix(from, qa);
  quat_from_matrix(to, qb);
  float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
  if (dot < 0.f) {
    for (float& value : qb) value = -value;
    dot = -dot;
  }
  if (dot > 0.9995f) {
    for (int i = 0; i < 4; ++i) mixed[i] = qa[i] + (qb[i] - qa[i]) * weight;
  } else {
    const float theta = std::acos(std::clamp(dot, -1.f, 1.f));
    const float sine = std::sin(theta);
    const float w0 = std::sin((1.f - weight) * theta) / sine;
    const float w1 = std::sin(weight * theta) / sine;
    for (int i = 0; i < 4; ++i) mixed[i] = qa[i] * w0 + qb[i] * w1;
  }
  const float length = std::sqrt(mixed[0] * mixed[0] + mixed[1] * mixed[1] +
                                 mixed[2] * mixed[2] + mixed[3] * mixed[3]);
  if (length > 1e-8f)
    for (float& value : mixed) value /= length;
  float translation[3];
  for (int i = 0; i < 3; ++i)
    translation[i] = from[12 + i] + (to[12 + i] - from[12 + i]) * weight;
  quat_to_matrix(out, mixed, translation);
}

bool same_name(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto left = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
    const auto right = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (left != right) return false;
  }
  return true;
}

Character* ensure_character(void* key, int32_t type_id, const RpkIndex* packs) {
  if (!key) return nullptr;
  Character& character = g_characters[key];
  if (type_id != 0 && character.type_id != type_id) {
    character.type_id = type_id;
    character.tried = false;
    character.bind.reset();
    character.fresh = false;
  }
  if (!character.tried && type_id != 0 && packs) {
    character.tried = true;
    character.bind = load_skeleton(*packs, render_skeleton(*packs, type_id));
    character.fresh = false;
    if (character.bind && character.overrides.size() < character.bind->bones.size())
      character.overrides.resize(character.bind->bones.size());
  }
  return &character;
}

bool any_override(const Character& character) {
  for (const BoneOverride& bone : character.overrides)
    if (bone.set) return true;
  return false;
}

void rebuild(Character& character, float now) {
  if (character.fresh && character.fresh_now == now) return;
  if (character.playing && character.clip && character.last_now >= 0.f && now >= character.last_now) {
    const float step = now - character.last_now;
    character.time += character.speed * step;
    character.fade_age += step;
    const float duration = character.clip->times.empty() ? 0.f : character.clip->times.back();
    if (character.loop && duration > 0.f) {
      character.time = std::fmod(character.time, duration);
      if (character.time < 0.f) character.time += duration;
    } else if (!character.loop) {
      if (character.time < 0.f) {
        character.time = 0.f;
        character.playing = false;
      } else if (duration > 0.f && character.time > duration) {
        character.time = duration;
        character.playing = false;
      }
    }
  }
  character.last_now = now;
  character.blend = 1.f;
  if (character.clip && character.fade > 0.f)
    character.blend = std::clamp(character.fade_age / character.fade, 0.f, 1.f);
  const bool posed = any_override(character) || (character.clip && character.blend > 0.f);
  character.want_skin = posed && character.bind != nullptr;
  if (character.bind) {
    const size_t count = character.bind->bones.size();
    std::vector<float> locals(count * 16);
    skeleton_local_bind(*character.bind, locals.data());
    if (character.clip && character.blend > 0.f) {
      std::vector<float> sampled = locals;
      clip_sample(*character.bind, character.lengths.get(), *character.clip, character.time,
                  sampled.data());
      if (character.blend < 1.f) {
        for (size_t i = 0; i < count; ++i)
          blend_local(locals.data() + i * 16, sampled.data() + i * 16, character.blend,
                      locals.data() + i * 16);
      } else {
        locals.swap(sampled);
      }
    }
    for (size_t i = 0; i < count && i < character.overrides.size(); ++i) {
      const BoneOverride& bone = character.overrides[i];
      if (bone.set) std::memcpy(locals.data() + i * 16, bone.local, sizeof(float) * 16);
    }
    std::vector<float> worlds(count * 16);
    skeleton_world(*character.bind, locals.data(), worlds.data());
    // The renderer keeps a GPU mesh per geometry pointer. A new pose needs a
    // new object; an unchanged pose keeps the one already uploaded.
    if (worlds != character.worlds) {
      character.worlds = std::move(worlds);
      ++character.pose_serial;
    }
  }
  character.fresh = true;
  character.fresh_now = now;
}

Character* animation_character(void* animation) {
  const auto bound = g_animations.find(animation);
  if (bound == g_animations.end()) return nullptr;
  const auto character = g_characters.find(bound->second);
  if (character == g_characters.end() || character->second.animation != animation)
    return nullptr;
  return &character->second;
}

void rigid_inverse(const float* m, float* out) {
  identity(out);
  out[0] = m[0];
  out[1] = m[4];
  out[2] = m[8];
  out[4] = m[1];
  out[5] = m[5];
  out[6] = m[9];
  out[8] = m[2];
  out[9] = m[6];
  out[10] = m[10];
  const float x = m[12], y = m[13], z = m[14];
  out[12] = -(out[0] * x + out[4] * y + out[8] * z);
  out[13] = -(out[1] * x + out[5] * y + out[9] * z);
  out[14] = -(out[2] * x + out[6] * y + out[10] * z);
}

void quat_normalize(float* q) {
  const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n < 1e-8f) {
    q[0] = q[1] = q[2] = 0.f;
    q[3] = 1.f;
    return;
  }
  for (int i = 0; i < 4; ++i) q[i] /= n;
}

void quat_mul(float* out, const float* a, const float* b) {
  const float r[4] = {
      a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
      a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
      a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
      a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  };
  std::memcpy(out, r, sizeof(r));
}

void quat_from_to(float* q, const float* from, const float* to) {
  const float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
  if (dot > 0.999999f) {
    q[0] = q[1] = q[2] = 0.f;
    q[3] = 1.f;
    return;
  }
  if (dot < -0.999999f) {
    const float ax = std::fabs(from[0]) < 0.9f ? 1.f : 0.f;
    const float ay = std::fabs(from[0]) < 0.9f ? 0.f : 1.f;
    q[0] = from[1] * 0.f - from[2] * ay;
    q[1] = from[2] * ax - from[0] * 0.f;
    q[2] = from[0] * ay - from[1] * ax;
    q[3] = 0.f;
    quat_normalize(q);
    return;
  }
  q[0] = from[1] * to[2] - from[2] * to[1];
  q[1] = from[2] * to[0] - from[0] * to[2];
  q[2] = from[0] * to[1] - from[1] * to[0];
  q[3] = 1.f + dot;
  quat_normalize(q);
}

void quat_limit(float* q, float max_radians) {
  if (!(max_radians > 0.f)) {
    q[0] = q[1] = q[2] = 0.f;
    q[3] = 1.f;
    return;
  }
  if (q[3] < 0.f)
    for (int i = 0; i < 4; ++i) q[i] = -q[i];
  const float theta = std::acos(std::clamp(q[3], -1.f, 1.f));
  const float angle = 2.f * theta;
  if (angle <= max_radians || angle < 1e-5f) return;
  const float t = max_radians / angle;
  const float sine = std::sin(theta);
  if (sine < 1e-6f) return;
  const float w0 = std::sin((1.f - t) * theta) / sine;
  const float w1 = std::sin(t * theta) / sine;
  q[0] *= w1;
  q[1] *= w1;
  q[2] *= w1;
  q[3] = w0 + w1 * q[3];
}

void store_local(Character& character, int bone, const float* local) {
  if (bone < 0) return;
  if (character.overrides.size() <= static_cast<size_t>(bone))
    character.overrides.resize(static_cast<size_t>(bone) + 1);
  BoneOverride& slot = character.overrides[static_cast<size_t>(bone)];
  slot.set = true;
  std::memcpy(slot.local, local, sizeof(float) * 16);
  character.fresh = false;
}

void detach_animation(void* animation, void* except) {
  const auto bound = g_animations.find(animation);
  if (bound == g_animations.end() || bound->second == except) return;
  const auto character = g_characters.find(bound->second);
  if (character != g_characters.end() && character->second.animation == animation) {
    character->second.clip.reset();
    character->second.playing = false;
    character->second.animation = nullptr;
    character->second.fresh = false;
  }
}

}  // namespace

void character_forget(void* key) {
  if (!key) return;
  g_characters.erase(key);
  g_links.erase(key);
  for (auto link = g_links.begin(); link != g_links.end();) {
    if (link->second.parent == key) link = g_links.erase(link);
    else ++link;
  }
  for (auto& [owner, character] : g_characters) {
    (void)owner;
    if (character.animation == key) {
      character.animation = nullptr;
      character.clip.reset();
      character.playing = false;
      character.fresh = false;
    }
  }
  for (auto animation = g_animations.begin(); animation != g_animations.end();) {
    if (animation->first == key || animation->second == key)
      animation = g_animations.erase(animation);
    else
      ++animation;
  }
}

void character_forget_all() {
  g_characters.clear();
  g_links.clear();
  g_animations.clear();
}

std::shared_ptr<const ScxGeometry> character_posed(void* key, int32_t type_id,
                                                   const ScxGeometry* bind,
                                                   const RpkIndex& packs) {
  Character* character = ensure_character(key, type_id, &packs);
  if (!character || !bind || bind->skin_weights.empty()) return nullptr;
  rebuild(*character, gameref_now());
  if (!character->want_skin || !character->bind ||
      character->worlds.size() < character->bind->bones.size() * 16)
    return nullptr;
  if (!character->posed || character->posed_from != bind) {
    character->posed = std::make_shared<ScxGeometry>(*bind);
    character->posed_from = bind;
    character->posed_serial = character->pose_serial + 1;
  }
  if (character->posed_serial == character->pose_serial) return character->posed;
  skeleton_skin(*bind, *character->bind, character->worlds.data(),
                &character->posed->positions, &character->posed->normals);
  character->posed_serial = character->pose_serial;
  character->posed->revision++;
  return character->posed;
}

int character_skin_palette(void* key, int32_t type_id, const RpkIndex& packs,
                           float* out, int capacity) {
  if (!out || capacity <= 0) return 0;
  Character* character = ensure_character(key, type_id, &packs);
  if (!character || !character->bind) return 0;
  rebuild(*character, gameref_now());
  if (character->worlds.size() < character->bind->bones.size() * 16) return 0;
  return skeleton_skin_palette(*character->bind, character->worlds.data(), out, capacity);
}

int character_find_bone(void* key, int32_t type_id, const RpkIndex& packs,
                        const std::string& name) {
  const Character* character = ensure_character(key, type_id, &packs);
  if (!character || !character->bind || name.empty()) return -1;
  for (size_t i = 0; i < character->bind->bones.size(); ++i)
    if (same_name(character->bind->bones[i].name, name)) return static_cast<int>(i);
  return -1;
}

int character_bone_id(void* key, int32_t type_id, const RpkIndex& packs,
                      const std::string& name) {
  const int id = character_find_bone(key, type_id, packs, name);
  return id < 0 ? 0 : id;
}

void character_set_bone_local(void* key, int32_t type_id, const RpkIndex& packs,
                              int bone, float x_m, float y_m, float z_m,
                              float yaw, float pitch, float roll) {
  if (!key || bone < 0) return;
  Character* character = ensure_character(key, type_id, &packs);
  if (!character) return;
  const float position[3] = {x_m * 100.f, y_m * 100.f, z_m * 100.f};
  const float ypr[3] = {yaw, pitch, roll};
  float local[16];
  ypr_matrix(local, position, ypr);
  store_local(*character, bone, local);
}

bool character_aim_bone(void* key, int32_t type_id, const RpkIndex& packs,
                        int bone, int end, const float target_cm[3], float max_radians) {
  Character* character = ensure_character(key, type_id, &packs);
  if (!character || !character->bind || !target_cm) return false;
  const int count = static_cast<int>(character->bind->bones.size());
  if (bone < 0 || end < 0 || bone >= count || end >= count || bone == end) return false;
  rebuild(*character, gameref_now());
  if (character->worlds.size() < static_cast<size_t>(count) * 16) return false;
  const float* worlds = character->worlds.data();
  const float* origin = worlds + bone * 16;
  const float* tip = worlds + end * 16;
  float from[3] = {tip[12] - origin[12], tip[13] - origin[13], tip[14] - origin[14]};
  float to[3] = {target_cm[0] - origin[12], target_cm[1] - origin[13], target_cm[2] - origin[14]};
  const float from_len = std::sqrt(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]);
  const float to_len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
  if (from_len < 1e-3f || to_len < 1e-3f) return false;
  for (int i = 0; i < 3; ++i) {
    from[i] /= from_len;
    to[i] /= to_len;
  }
  float correction[4];
  quat_from_to(correction, from, to);
  quat_limit(correction, max_radians);
  float world_q[4];
  quat_from_matrix(origin, world_q);
  float turned[4];
  quat_mul(turned, correction, world_q);
  const float translation[3] = {origin[12], origin[13], origin[14]};
  float world[16];
  quat_to_matrix(world, turned, translation);
  const int parent = character->bind->bones[static_cast<size_t>(bone)].parent;
  float local[16];
  if (parent < 0) {
    std::memcpy(local, world, sizeof(local));
  } else {
    float inverse[16];
    rigid_inverse(worlds + parent * 16, inverse);
    multiply(local, inverse, world);
  }
  store_local(*character, bone, local);
  return true;
}

bool character_align_axis(void* key, int32_t type_id, const RpkIndex& packs,
                          int bone, int axis, const float direction[3]) {
  Character* character = ensure_character(key, type_id, &packs);
  if (!character || !character->bind || !direction || axis < 0 || axis > 2) return false;
  const int count = static_cast<int>(character->bind->bones.size());
  if (bone < 0 || bone >= count) return false;
  rebuild(*character, gameref_now());
  if (character->worlds.size() < static_cast<size_t>(count) * 16) return false;
  const float* origin = character->worlds.data() + bone * 16;
  float from[3] = {origin[axis * 4], origin[axis * 4 + 1], origin[axis * 4 + 2]};
  float to[3] = {direction[0], direction[1], direction[2]};
  const float from_len = std::sqrt(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]);
  const float to_len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
  if (from_len < 1e-6f || to_len < 1e-6f) return false;
  for (int i = 0; i < 3; ++i) {
    from[i] /= from_len;
    to[i] /= to_len;
  }
  float correction[4];
  quat_from_to(correction, from, to);
  float world_q[4];
  quat_from_matrix(origin, world_q);
  float turned[4];
  quat_mul(turned, correction, world_q);
  const float translation[3] = {origin[12], origin[13], origin[14]};
  float world[16];
  quat_to_matrix(world, turned, translation);
  const int parent = character->bind->bones[static_cast<size_t>(bone)].parent;
  float local[16];
  if (parent < 0) {
    std::memcpy(local, world, sizeof(local));
  } else {
    float inverse[16];
    rigid_inverse(character->worlds.data() + parent * 16, inverse);
    multiply(local, inverse, world);
  }
  store_local(*character, bone, local);
  return true;
}

bool character_solve_limb(void* key, int32_t type_id, const RpkIndex& packs,
                          int upper, int lower, int end, const float target_cm[3],
                          const float pole[3]) {
  Character* character = ensure_character(key, type_id, &packs);
  if (!character || !character->bind || !target_cm || !pole) return false;
  const int count = static_cast<int>(character->bind->bones.size());
  if (upper < 0 || lower < 0 || end < 0 || upper >= count || lower >= count || end >= count)
    return false;
  rebuild(*character, gameref_now());
  if (character->worlds.size() < static_cast<size_t>(count) * 16) return false;
  const auto at = [&](int bone, float* out) {
    const float* m = character->worlds.data() + bone * 16;
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
  };
  float a[3], b[3], c[3];
  at(upper, a);
  at(lower, b);
  at(end, c);
  const auto length = [](const float* p, const float* q) {
    const float d0 = q[0] - p[0], d1 = q[1] - p[1], d2 = q[2] - p[2];
    return std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
  };
  const float l1 = length(a, b);
  const float l2 = length(b, c);
  float direction[3] = {target_cm[0] - a[0], target_cm[1] - a[1], target_cm[2] - a[2]};
  const float reach = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                direction[2] * direction[2]);
  if (l1 < 1e-3f || l2 < 1e-3f || reach < 1e-3f) return false;
  const float distance = std::clamp(reach, std::fabs(l1 - l2) + 0.05f, l1 + l2 - 0.05f);
  for (float& axis : direction) axis /= reach;
  float bend[3] = {pole[0], pole[1], pole[2]};
  const float along_pole = bend[0] * direction[0] + bend[1] * direction[1] + bend[2] * direction[2];
  for (int i = 0; i < 3; ++i) bend[i] -= direction[i] * along_pole;
  float bend_len = std::sqrt(bend[0] * bend[0] + bend[1] * bend[1] + bend[2] * bend[2]);
  if (bend_len < 1e-4f) {
    bend[0] = 1.f - direction[0] * direction[0];
    bend[1] = -direction[1] * direction[0];
    bend[2] = -direction[2] * direction[0];
    bend_len = std::sqrt(bend[0] * bend[0] + bend[1] * bend[1] + bend[2] * bend[2]);
    if (bend_len < 1e-4f) return false;
  }
  for (float& axis : bend) axis /= bend_len;
  const float along = (l1 * l1 - l2 * l2 + distance * distance) / (2.f * distance);
  const float height = std::sqrt(std::max(0.f, l1 * l1 - along * along));
  const float elbow[3] = {a[0] + direction[0] * along + bend[0] * height,
                          a[1] + direction[1] * along + bend[1] * height,
                          a[2] + direction[2] * along + bend[2] * height};
  const float hand[3] = {a[0] + direction[0] * distance, a[1] + direction[1] * distance,
                         a[2] + direction[2] * distance};
  character_aim_bone(key, type_id, packs, upper, lower, elbow, 3.14159265f);
  character_aim_bone(key, type_id, packs, lower, end, hand, 3.14159265f);
  return true;
}

bool character_place_bone(void* key, int32_t type_id, const RpkIndex& packs, int bone,
                          const float position_m[3]) {
  Character* character = ensure_character(key, type_id, &packs);
  if (!character || !character->bind || !position_m) return false;
  const int count = static_cast<int>(character->bind->bones.size());
  if (bone < 0 || bone >= count) return false;
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(position_m[i])) return false;
  // Bind orientation in render space, with the requested origin.
  std::vector<float> locals(static_cast<size_t>(count) * 16), worlds(locals.size());
  skeleton_local_bind(*character->bind, locals.data());
  skeleton_world(*character->bind, locals.data(), worlds.data());
  float world[16];
  std::memcpy(world, worlds.data() + bone * 16, sizeof(world));
  for (int i = 0; i < 3; ++i) world[12 + i] = position_m[i] * 100.f;
  const int parent = character->bind->bones[static_cast<size_t>(bone)].parent;
  float local[16];
  if (parent < 0 || parent >= count) {
    std::memcpy(local, world, sizeof(local));
  } else {
    // Relative to the parent as it is posed now, so a parent placed first
    // keeps its child where it was asked to go.
    rebuild(*character, gameref_now());
    if (character->worlds.size() < static_cast<size_t>(count) * 16) return false;
    float inverse[16];
    rigid_inverse(character->worlds.data() + parent * 16, inverse);
    multiply(local, inverse, world);
  }
  // A per-tick refresh with an unchanged pose leaves the skinned mesh alone.
  if (static_cast<size_t>(bone) < character->overrides.size()) {
    const BoneOverride& current = character->overrides[static_cast<size_t>(bone)];
    if (current.set && std::memcmp(current.local, local, sizeof(local)) == 0) return true;
  }
  store_local(*character, bone, local);
  return true;
}

void character_reset_pose(void* key) {
  if (!key) return;
  const auto found = g_characters.find(key);
  if (found == g_characters.end()) return;
  for (BoneOverride& bone : found->second.overrides) bone.set = false;
  found->second.fresh = false;
  found->second.want_skin = false;
}

void character_link_bone(void* child, void* parent, int bone) {
  if (!child || !parent || child == parent || bone < 0) return;
  g_links[child] = BoneLink{parent, bone};
}

void character_unlink(void* child) {
  if (child) g_links.erase(child);
}

bool character_linked_bone(void* child, void** parent, int* bone) {
  const auto link = g_links.find(child);
  if (link == g_links.end()) return false;
  if (parent) *parent = link->second.parent;
  if (bone) *bone = link->second.bone;
  return true;
}

bool character_bone_metres(void* owner, int bone, float out[16]) {
  if (!owner || !out || bone < 0) return false;
  const auto found = g_characters.find(owner);
  if (found == g_characters.end() || !found->second.bind) return false;
  Character& character = found->second;
  rebuild(character, gameref_now());
  const size_t count = character.bind->bones.size();
  if (static_cast<size_t>(bone) >= count || character.worlds.size() < count * 16)
    return false;
  std::memcpy(out, character.worlds.data() + static_cast<size_t>(bone) * 16,
              sizeof(float) * 16);
  out[12] *= 0.01f;
  out[13] *= 0.01f;
  out[14] *= 0.01f;
  return true;
}

void character_bind_animation(void* animation, void* render, int32_t clip_id,
                              const RpkIndex& packs) {
  if (!animation || !render) return;
  detach_animation(animation, render);
  const GameRefState* state = gameref_get(render);
  const int32_t type_id = state ? state->type_id : 0;
  Character* character = ensure_character(render, type_id, &packs);
  if (!character) return;
  if (character->animation && character->animation != animation)
    g_animations.erase(character->animation);
  character->animation = animation;
  g_animations[animation] = render;
  const std::string source = packs.payload_prop(static_cast<uint32_t>(clip_id), "sourcefile");
  character->clip = load_clip(packs, source);
  character->lengths = load_skeleton(packs, sibling_bon(source));
  character->speed = 1.f;
  character->fade = 0.f;
  character->fade_age = 0.f;
  character->time = 0.f;
  character->playing = false;
  character->loop = false;
  character->last_now = -1.f;
  character->fresh = false;
}

void character_release_animation(void* animation) {
  if (Character* character = animation_character(animation)) {
    character->clip.reset();
    character->playing = false;
    character->animation = nullptr;
    character->fresh = false;
  }
  g_animations.erase(animation);
}

void character_set_speed(void* animation, float speed) {
  if (Character* character = animation_character(animation)) {
    if (!std::isfinite(speed)) return;
    character->speed = speed;
    character->fresh = false;
  }
}

void character_set_fade(void* animation, float fade) {
  if (Character* character = animation_character(animation)) {
    if (!std::isfinite(fade) || fade < 0.f) return;
    character->fade = fade;
    character->fade_age = 0.f;
    character->fresh = false;
  }
}

void character_play(void* animation, bool loop) {
  if (Character* character = animation_character(animation)) {
    character->playing = true;
    character->loop = loop;
    character->fade_age = 0.f;
    character->last_now = -1.f;
    character->fresh = false;
  }
}

void character_pause(void* animation) {
  if (Character* character = animation_character(animation)) {
    character->playing = false;
    character->fresh = false;
  }
}

bool character_seek(void* animation, float seconds) {
  Character* character = animation_character(animation);
  if (!character || !std::isfinite(seconds) || seconds < 0.f) return false;
  character->time = seconds;
  character->fresh = false;
  return true;
}

float character_position(void* animation) {
  const Character* character = animation_character(animation);
  return character ? character->time : 0.f;
}

float character_duration(void* animation) {
  const Character* character = animation_character(animation);
  if (!character || !character->clip || character->clip->times.empty()) return 0.f;
  return character->clip->times.back();
}

}  // namespace slrr
