#include "slrr/frame_profiler.hpp"
#include "standalone_test.hpp"
using namespace slrr;
void snapshot_contract() {
  perf::enable("");
  perf::restart(1);
  SLRR_REQUIRE(!perf::snapshot().ready);
  perf::end_frame();
  perf::add(perf::Zone::Traffic, 2000000);
  perf::add(perf::Zone::Physics, 3000000);
  perf::end_frame();
  auto s = perf::snapshot();
  SLRR_REQUIRE(s.ready && s.zones[size_t(perf::Zone::Traffic)] == 2 &&
               s.zones[size_t(perf::Zone::Physics)] == 3);
  perf::end_frame();
  s = perf::snapshot();
  SLRR_REQUIRE(s.ready && s.zones[size_t(perf::Zone::Traffic)] == 0);
  perf::restart(1);
  SLRR_REQUIRE(!perf::snapshot().ready);
}
int main() {
  const standalone::Test tests[] = {snapshot_contract};
  return standalone::run("frame_profiler", tests);
}
