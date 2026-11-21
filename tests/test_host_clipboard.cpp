// Windows clipboard round trip used by the renderer debug menu (#243). The
// user's clipboard text is saved first and restored at the end.
#include "slrr/host_clipboard.hpp"
#include "standalone_test.hpp"

using namespace slrr;
namespace {
SLRR_TEST(round_trip_utf8) {
  const std::string saved = host_clipboard_get_text();
  const std::string sample = "view | name | GPU ms\n  5 | World | 0.130 \xC3\xA9";
  SLRR_REQUIRE(host_clipboard_set_text(sample));
  const std::string read = host_clipboard_get_text();
  SLRR_REQUIRE(host_clipboard_set_text(""));
  SLRR_REQUIRE(host_clipboard_get_text().empty());
  if (!saved.empty()) host_clipboard_set_text(saved);
  SLRR_REQUIRE(read == sample);
}
} // namespace

int main() {
  const standalone::Test tests[] = {round_trip_utf8};
  return standalone::run("host_clipboard", tests);
}
