#include "standalone_test.hpp"

#include "slrr/trigger_volume.hpp"

#include <cmath>

namespace {

using slrr::TriggerShape;
using slrr::TriggerVolume;
using slrr::trigger_volume_contains;
using slrr::trigger_volume_parse;

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

}  // namespace

SLRR_TEST(stock_trigger_params_parse_spheres_and_boxes) {
  // java.game.Trigger: pos.toString() + ",0,0,0,sphere," + r
  TriggerVolume sphere;
  SLRR_REQUIRE(trigger_volume_parse("-278.518,9.8,1033.002,0,0,0,sphere,6.0", sphere));
  SLRR_REQUIRE(sphere.shape == TriggerShape::Sphere);
  SLRR_REQUIRE(near(sphere.pose[0], -278.518f));
  SLRR_REQUIRE(near(sphere.pose[1], 9.8f));
  SLRR_REQUIRE(near(sphere.pose[2], 1033.002f));
  SLRR_REQUIRE(near(sphere.size[0], 6.f));

  TriggerVolume box;
  SLRR_REQUIRE(trigger_volume_parse(" 814, 0, -1109 ,0,0,0, box ,30,300,30", box));
  SLRR_REQUIRE(box.shape == TriggerShape::Box);
  SLRR_REQUIRE(near(box.pose[2], -1109.f));
  SLRR_REQUIRE(near(box.size[0], 30.f));
  SLRR_REQUIRE(near(box.size[1], 300.f));
  SLRR_REQUIRE(near(box.size[2], 30.f));
}

SLRR_TEST(malformed_trigger_params_are_rejected_without_output) {
  TriggerVolume volume;
  volume.size[0] = 42.f;
  for (const char* params : {"", "1,2,3", "1,2,3,0,0,0,sphere", "1,2,3,0,0,0,sphere,0",
                             "1,2,3,0,0,0,sphere,-1", "1,2,3,0,0,0,cylinder,4",
                             "1,2,3,0,0,0,box,1,2", "a,2,3,0,0,0,sphere,4",
                             "1,2,3,0,0,0,sphere,nan", "1,2,3x,0,0,0,sphere,4"}) {
    SLRR_REQUIRE(!trigger_volume_parse(params, volume));
    SLRR_REQUIRE(volume.shape == TriggerShape::None);
    SLRR_REQUIRE(near(volume.size[0], 42.f));
  }
}

SLRR_TEST(trigger_contains_uses_sphere_radius_and_box_half_extents) {
  const float radius[3] = {6.f, 0.f, 0.f};
  const float inside[3] = {4.f, 4.f, 2.f}, outside[3] = {4.f, 4.f, 2.1f};  // 6 m, just over
  SLRR_REQUIRE(trigger_volume_contains(TriggerShape::Sphere, radius, inside));
  SLRR_REQUIRE(!trigger_volume_contains(TriggerShape::Sphere, radius, outside));

  const float half[3] = {1.f, 300.f, 2000.f};
  const float on_face[3] = {1.f, -300.f, 0.f}, beyond[3] = {1.01f, 0.f, 0.f};
  SLRR_REQUIRE(trigger_volume_contains(TriggerShape::Box, half, on_face));
  SLRR_REQUIRE(!trigger_volume_contains(TriggerShape::Box, half, beyond));
  SLRR_REQUIRE(!trigger_volume_contains(TriggerShape::None, half, on_face));
}

SLRR_TEST(trigger_contacts_raise_edges_only_for_cars_that_drive_across) {
  using slrr::TriggerContact;
  using slrr::TriggerEdge;
  using slrr::trigger_contact_update;
  TriggerContact contact;
  // A car spawned inside a garage trigger has not entered it.
  SLRR_REQUIRE(trigger_contact_update(contact, true, false) == TriggerEdge::None);
  SLRR_REQUIRE(trigger_contact_update(contact, true, false) == TriggerEdge::None);
  SLRR_REQUIRE(trigger_contact_update(contact, false, false) == TriggerEdge::Leave);
  SLRR_REQUIRE(trigger_contact_update(contact, false, false) == TriggerEdge::None);
  SLRR_REQUIRE(trigger_contact_update(contact, true, false) == TriggerEdge::Enter);
  // A teleport out and back in is silent both ways.
  SLRR_REQUIRE(trigger_contact_update(contact, false, true) == TriggerEdge::None);
  SLRR_REQUIRE(!contact.inside);
  SLRR_REQUIRE(trigger_contact_update(contact, true, true) == TriggerEdge::None);
  SLRR_REQUIRE(contact.inside);

  TriggerContact outside;
  SLRR_REQUIRE(trigger_contact_update(outside, false, false) == TriggerEdge::None);
  SLRR_REQUIRE(trigger_contact_update(outside, true, false) == TriggerEdge::Enter);
}

int main() {
  const slrr::standalone::Test tests[] = {
      stock_trigger_params_parse_spheres_and_boxes,
      malformed_trigger_params_are_rejected_without_output,
      trigger_contains_uses_sphere_radius_and_box_half_extents,
      trigger_contacts_raise_edges_only_for_cars_that_drive_across,
  };
  return slrr::standalone::run("trigger_volume", tests);
}
