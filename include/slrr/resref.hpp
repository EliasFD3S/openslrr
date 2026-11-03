#pragma once

#include "slrr/scx_mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// Soft PE ResourceRef / GameRef / RenderRef handle state (Runtime Resources).
// Keyed by java Object* — same model as slrr-reverse g_res / g_refs.

constexpr int32_t kResTypeInvalid = 0;
constexpr int32_t kResTypeInstanceGame = 1;
constexpr int32_t kResTypeInstancePhysics = 2;
constexpr int32_t kResTypeInstanceRender = 3;

struct ResState {
  void* key = nullptr;
  void* parent = nullptr;
  int32_t id = 0;
  int32_t parent_id = 0;
  int32_t type = kResTypeInvalid;  // RESTYPE / INSTANCE_*
  int32_t type_id = 0;             // RenderRef type resource id
  int32_t flags = 0;
  bool loaded = false;
  bool empty = true;  // GameRef.isEmpty
  std::string alias;
  std::string entry_path;
  std::string class_name;
  void* script = nullptr;  // GameType script instance
  float pos[3] = {0, 0, 0};
  float ypr[3] = {0, 0, 0};
  float vel[3] = {0, 0, 0};
  std::vector<void*> children;
  // RenderRef extras
  bool has_mesh = false;
  ScxGeometry mesh;
};

void resref_clear();
ResState* resref_upsert(void* key);
ResState* resref_get(void* key);
void resref_erase(void* key);

void resref_new_native(void* key);
void resref_delete_native(void* key);
void resref_set_id(void* key, int32_t id);
void resref_set_from(void* key, void* other);
int32_t resref_id(void* key);
int32_t resref_type(void* key);
void resref_set_parent(void* key, void* parent);
void* resref_parent(void* key);
void* resref_first_child(void* key);
void* resref_next_child(void* key, void* after);
void* resref_wt_root(void* key);

// GameRef / RenderRef Soft create paths.
void gameref_create_native(void* self, void* parent, void* type,
                           const char* params, const char* alias);
void* gameref_create(void* self, void* parent, void* type, const char* params,
                     const char* alias);  // returns script GameType or self
void renderref_create(void* self, void* parent, void* type, const char* alias);
void renderref_set_matrix(void* self, float px, float py, float pz, float yaw,
                          float pitch, float roll);
void renderref_set_type(void* self, void* type_ref);

}  // namespace slrr
