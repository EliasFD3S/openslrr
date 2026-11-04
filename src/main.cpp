#include "slrr/boot.hpp"
#include "slrr/game_root.hpp"
#include "slrr/jvm.hpp"
#include "slrr/native_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/sl2_pacejka.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage() {
  std::printf(
      "slrr_host — clean C++ host for Street Legal Racing Redline\n"
      "\n"
      "Usage:\n"
      "  slrr_host --probe [--root DIR]\n"
      "  slrr_host --probe-pacejka\n"
      "  slrr_host --game [--root DIR] [--window] [--no-wait] [--auto-new]\n"
      "           [--auto-cmd mainmenu|street|N] [--render null|d3d9|bgfx]\n"
      "           [--spawn-car cars/racers/Baiern.rpk:0x6]\n"
      "           [PlayerName]\n"
      "\n"
      "Default --game: FMV intros → Init(int) (Soft splash / menu).\n"
      "--auto-new skips to Garage (smoke). ESC skips boot intros.\n"
      "Game data: open-SL/game or SLRR_GAME_ROOT / --root.\n"
      "Reference only: open-SL/repos (do not edit as product code).\n");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace slrr;
  // Line-buffered stdout stalls a dialog on every log line. Keep a block
  // buffer; real failures still go to stderr.
  std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

  bool want_probe = false;
  bool want_pacejka = false;
  bool want_game = false;
  BootOptions opt;
  const char* root_arg = nullptr;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!a) continue;
    if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(a, "--probe") == 0) {
      want_probe = true;
      continue;
    }
    if (std::strcmp(a, "--probe-pacejka") == 0) {
      want_pacejka = true;
      continue;
    }
    if (std::strcmp(a, "--game") == 0) {
      want_game = true;
      continue;
    }
    if (std::strcmp(a, "--window") == 0) {
      opt.window = true;
      continue;
    }
    if (std::strcmp(a, "--no-wait") == 0) {
      opt.auto_new = true;
      opt.max_frames = 3;
      continue;
    }
    if (std::strcmp(a, "--auto-new") == 0) {
      opt.auto_new = true;
      continue;
    }
    if(std::strcmp(a,"--spawn-car")==0&&i+1<argc){
      opt.spawn_car=argv[++i];opt.auto_new=true;continue;
    }
    if (std::strcmp(a, "--auto-cmd") == 0 && i + 1 < argc) {
      const char* c = argv[++i];
      if (std::strcmp(c, "mainmenu") == 0)
        opt.auto_cmd = 101;
      else if (std::strcmp(c, "street") == 0)
        opt.auto_cmd = 109;
      else if (std::strcmp(c, "escape") == 0)
        opt.auto_cmd = 119;
      else
        opt.auto_cmd = static_cast<int32_t>(std::atoi(c));
      continue;
    }
    if (std::strcmp(a, "--root") == 0 && i + 1 < argc) {
      root_arg = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--render") == 0 && i + 1 < argc) {
      opt.render = parse_render_backend(argv[++i]);
      continue;
    }
    if (a[0] != '-') {
      opt.player_name = a;
      continue;
    }
    std::fprintf(stderr, "unknown arg: %s\n", a);
    usage();
    return 1;
  }

  if (!want_probe && !want_game && !want_pacejka) {
    usage();
    return 1;
  }

  if (want_pacejka) {
    char msg[256];
    const bool ok = sl2_pacejka_self_check(msg, sizeof(msg));
    std::printf("[phys] %s\n", msg);
    if (!ok) return 3;
    if (!want_probe && !want_game) return 0;
  }

  if (const char* env = std::getenv("SLRR_GAME_ROOT")) {
    if (!root_arg) root_arg = env;
  }

  opt.game_root = find_game_root(root_arg);
  if (opt.game_root.empty()) {
    std::fprintf(stderr,
                 "FAIL: game root not found (need system/Scripts/lang/System.class)\n"
                 "  try: --root ..\\game   or set SLRR_GAME_ROOT\n");
    return 1;
  }
  std::printf("[host] game_root=%s\n", opt.game_root.c_str());

  NativeRegistry natives;
  Jvm jvm(&natives);

  if (want_probe && !want_game) return boot_probe(jvm, opt);

  auto render = create_render_backend(opt.render);
  return boot_game(jvm, *render, opt);
}
