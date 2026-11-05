#include "slrr/host_input.hpp"
// This executable isolates resource/scene transforms. Camera interaction and
// vehicle ownership are covered by host_runtime_test with their real modules.
namespace slrr {
HostInput &host_input() {
  static HostInput input;
  return input;
}
bool part_car_ground(void *, float *) { return false; }
} // namespace slrr
