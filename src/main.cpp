#include "slrr/boot.hpp"
#include "slrr/game_root.hpp"
#include "slrr/jvm.hpp"
#include "slrr/native_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"
#include "slrr/physics_telemetry.hpp"
#include "slrr/sl2_pacejka.hpp"
#include "slrr/natives_boot.hpp"
#include "slrr/gameref.hpp"
#include "slrr/crash_logging.hpp"
#include "slrr_version.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct DiagnosticsSession {
  ~DiagnosticsSession() {
    write_native_report();
    write_metrics();
    std::string telemetry_error;
    if (!slrr::physics_telemetry::shutdown(telemetry_error) && !telemetry_error.empty())
      std::fprintf(stderr, "FAIL: %s\n", telemetry_error.c_str());
    slrr::diagnostics::shutdown();
  }

  void write_metrics() noexcept {
    try {
      if (metrics_written || metrics_path.empty()) return;
      metrics_written = true;
      std::string error;
      if (!slrr::metrics::write_snapshot(metrics_path, error))
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
    } catch (...) {
      // Metrics finalization must not replace the host's return behavior.
    }
  }

  void write_native_report() noexcept {
    try {
      if (native_report_written || native_report_path.empty()) return;
      native_report_written = true;
      std::string error;
      const bool written = natives
          ? natives->write_report(native_report_path, error)
          : slrr::NativeRegistry().write_report(native_report_path, error);
      if (!written) std::fprintf(stderr, "FAIL: %s\n", error.c_str());
    } catch (...) {
      // Report finalization must not replace the host's return behavior.
    }
  }

  std::string metrics_path;
  bool metrics_written = false;
  std::string native_report_path;
  const slrr::NativeRegistry* natives = nullptr;
  bool native_report_written = false;
};

// The outer session also covers argument/probe early returns. This nested
// finalizer takes the normal VM-era snapshot before Jvm unregisters its provider.
struct MetricsFinalizer {
  explicit MetricsFinalizer(DiagnosticsSession& session) : session_(session) {}
  ~MetricsFinalizer() { session_.write_metrics(); }
  DiagnosticsSession& session_;
};

struct CrashLogSession {
  ~CrashLogSession() {
    if (slrr::crash_logging::active())
      slrr::crash_logging::shutdown(slrr::crash_logging::Disposition::Normal);
  }
};

struct CrashLogFinalizer {
  ~CrashLogFinalizer() {
    if (slrr::crash_logging::active())
      slrr::crash_logging::shutdown(slrr::crash_logging::Disposition::Normal);
  }
};

struct NativeReportFinalizer {
  explicit NativeReportFinalizer(DiagnosticsSession& session) : session_(session) {}
  ~NativeReportFinalizer() { session_.write_native_report(); }
  DiagnosticsSession& session_;
};


using Bytes = std::vector<std::uint8_t>;

void append_u32(Bytes& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_chunk(Bytes& out, const char (&tag)[5], const Bytes& payload) {
  out.insert(out.end(), tag, tag + 4);
  append_u32(out, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
}
void append_utf8(Bytes& out, const std::string& text) {
  append_u32(out, 0);
  append_u32(out, static_cast<std::uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0);
}
struct NativeMethod {
  const char* name;
  const char* descriptor;
  bool is_static;
};

Bytes hermetic_native_class(const std::string& name, const std::string& super,
                            const std::vector<NativeMethod>& methods) {
  Bytes cons;
  append_u32(cons, static_cast<std::uint32_t>(2 + (super.empty() ? 0 : 2) + methods.size() * 2));
  append_utf8(cons, name);
  append_u32(cons, 4);
  append_u32(cons, 0);
  if (!super.empty()) {
    append_utf8(cons, super);
    append_u32(cons, 4);
    append_u32(cons, 2);
  }
  const std::uint32_t first_method = super.empty() ? 2 : 4;
  for (const auto& method : methods) {
    append_utf8(cons, method.name);
    append_utf8(cons, method.descriptor);
  }
  Bytes method_data;
  const auto append_methods = [&](bool is_static) {
    std::uint32_t count = 0;
    for (const auto& method : methods) count += method.is_static == is_static;
    append_u32(method_data, count);
    for (std::size_t i = 0; i < methods.size(); ++i) {
      if (methods[i].is_static != is_static) continue;
      append_u32(method_data, 0x40);
      append_u32(method_data, first_method + static_cast<std::uint32_t>(i * 2));
      append_u32(method_data, first_method + static_cast<std::uint32_t>(i * 2 + 1));
      append_u32(method_data, slrr::kTufaNone);
      append_u32(method_data, 0);
    }
  };
  append_methods(true);
  append_methods(false);
  Bytes clss;
  append_u32(clss, slrr::kTufaNone);
  append_u32(clss, 0);
  append_u32(clss, 1);
  append_u32(clss, super.empty() ? slrr::kTufaNone : 3);
  append_u32(clss, 0);
  Bytes trees;
  append_u32(trees, 0);
  Bytes bytes;
  bytes.insert(bytes.end(), {'T','U','F','A'});
  append_u32(bytes, 4);
  append_u32(bytes, 940);
  append_chunk(bytes, "CONS", cons);
  append_chunk(bytes, "MTHD", method_data);
  append_chunk(bytes, "CLSS", clss);
  append_chunk(bytes, "TREE", trees);
  return bytes;
}

bool write_class(const std::filesystem::path& directory, const std::string& name,
                 const std::string& super, const std::vector<NativeMethod>& methods,
                 std::string& error) {
  const auto simple = name.substr(name.find_last_of('.') + 1);
  const auto path = directory / (simple + ".class");
  const auto bytes = hermetic_native_class(name, super, methods);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    error = "cannot write hermetic GC class: " + path.string();
    return false;
  }
  return true;
}

bool prepare_hermetic_gc_classes(slrr::Jvm& jvm, std::string& error) {
  namespace fs = std::filesystem;
  fs::path directory;
  if (const auto* paths = slrr::crash_logging::paths())
    directory = paths->directory / "hermetic-classes";
  else {
    std::error_code ec;
    directory = fs::temp_directory_path(ec) / "slrr-hermetic-gc-classes";
    if (ec) {
      error = "cannot select hermetic GC class directory";
      return false;
    }
  }
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) {
    error = "cannot create hermetic GC class directory: " + directory.string();
    return false;
  }
  const std::vector<NativeMethod> object_methods = {
      {"finalize", "()V", false}, {"disableGC", "()V", false},
      {"enableGC", "()V", false}, {"hashCode", "()I", false}};
  const std::vector<NativeMethod> string_methods = {{"finalize", "()V", false}};
  const std::vector<NativeMethod> system_methods = {{"gc", "()I", true}};
  if (!write_class(directory, "java.lang.Object", "", object_methods, error) ||
      !write_class(directory, "java.lang.String", "java.lang.Object", string_methods, error) ||
      !write_class(directory, "java.lang.System", "java.lang.Object", system_methods, error))
    return false;
  for (const char* name : {"Object", "String", "System"}) {
    if (!jvm.load_class_file((directory / (std::string(name) + ".class")).string())) {
      error = std::string("cannot load hermetic GC class: ") + name;
      return false;
    }
  }
  return true;
}

#ifndef SLRR_BUILD_CONFIGURATION
#define SLRR_BUILD_CONFIGURATION "Unknown"
#endif

void usage() {
  std::printf(
      "slrr_host — clean C++ host for Street Legal Racing Redline\n"
      "\n"
      "Usage:\n"
      "  slrr_host --probe [--root DIR]\n"
      "  slrr_host --probe-pacejka\n"
      "  slrr_host --probe-dyno\n"
      "  slrr_host --probe-gc [--root DIR] (hermetic without game data)\n"
      "  slrr_host --probe-physics [--physics-telemetry PATH]\n"
      "  slrr_host --compare-physics-replay LEFT RIGHT\n"
      "           [--physics-tolerance ABS] [--physics-compare-report PATH]\n"
      "  slrr_host --game [--root DIR] [--window] [--no-wait] [--auto-new]\n"
      "           [--auto-cmd mainmenu|street|N]\n"
      "           [--spawn-car cars/racers/Baiern.rpk:0x6]\n"
      "           [--vehicle-osd PATH]\n"
      "           [--perf-log CSV|-] [--bench-map TRACK] [--bench-drive SECONDS]\n"
      "           [--bench-time HOUR] [--bench-shots DIR]\n"
      "           [--no-vsync] [--debug-overlay]\n"
      "           [PlayerName]\n"
      "\n"
      "Diagnostics: --trace PATH writes structured JSONL (opt-in).\n"
      "             --metrics PATH writes a final JSON metrics snapshot;\n"
      "             includes bounded section history, even without --trace.\n"
      "             --native-report PATH writes bounded native coverage JSON;\n"
      "             no --trace or --metrics is required.\n"
      "             registered=bindings; called=unique attempted signatures;\n"
      "             no-op=explicit native_noop bindings; missing=failed lookups.\n"
      "             Registered bodies are not behaviorally verified.\n"
      "             --physics-telemetry PATH writes bounded physics JSONL.\n"
      "             --log-all DIR captures stdout/stderr, events, metrics, and crashes.\n"
      "             With --native-report, log-all owns DIR/native-report.json\n"
      "             and the PATH argument is not used.\n"
      "Replay compare: exit 0 = match, 4 = state mismatch, 2 = input/error.\n"
      "\n"
      "Default --game: FMV intros → Init(int) (Soft splash / menu).\n"
      "--auto-new skips to Garage (smoke). ESC skips boot intros.\n"
      "Game data: data/game, or SLRR_GAME_ROOT / --root.\n"
      "Reference only: open-SL/repos (do not edit as product code).\n");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace slrr;
  DiagnosticsSession diagnostics_session;
  CrashLogSession crash_log_session;
  // Line-buffered stdout stalls a dialog on every log line. Keep a block
  // buffer; real failures still go to stderr.
  std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

  bool want_probe = false;
  bool want_pacejka = false;
  bool want_dyno = false;
  bool want_game = false;
  bool want_gc = false;
  bool want_physics = false;
  BootOptions opt;
  const char* root_arg = nullptr;
  const char* trace_path = nullptr;
  const char* log_all_path = nullptr;
  const char* physics_telemetry_path = nullptr;
  const char* compare_left = nullptr;
  const char* compare_right = nullptr;
  const char* compare_report = nullptr;
  double physics_tolerance = 1e-9;

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
    if (std::strcmp(a, "--probe-dyno") == 0) {
      want_dyno = true;
      continue;
    }
    if (std::strcmp(a, "--probe-gc") == 0) {
      want_gc = true;
      continue;
    }
    if (std::strcmp(a, "--probe-physics") == 0) {
      want_physics = true;
      continue;
    }
    if (std::strcmp(a, "--compare-physics-replay") == 0) {
      if (i + 2 >= argc) {
        std::fprintf(stderr, "missing LEFT RIGHT for --compare-physics-replay\n");
        return 2;
      }
      compare_left = argv[++i];
      compare_right = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--physics-tolerance") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --physics-tolerance\n");
        return 2;
      }
      const char* value = argv[++i];
      char* end = nullptr;
      physics_tolerance = std::strtod(value, &end);
      if (!end || *end || !std::isfinite(physics_tolerance) ||
          physics_tolerance < 0 || physics_tolerance > 1) {
        std::fprintf(stderr, "invalid --physics-tolerance (expected 0..1)\n");
        return 2;
      }
      continue;
    }
    if (std::strcmp(a, "--physics-compare-report") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing path for --physics-compare-report\n");
        return 2;
      }
      compare_report = argv[++i];
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
    if (std::strcmp(a, "--vehicle-osd") == 0 && i + 1 < argc) {
      opt.vehicle_osd = argv[++i];
      continue;
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
    if (std::strcmp(a, "--perf-log") == 0 && i + 1 < argc) {
      opt.perf_log = true;
      const char* path = argv[++i];
      if (std::strcmp(path, "-") != 0) opt.perf_log_path = path;
      continue;
    }
    if (std::strcmp(a, "--no-vsync") == 0) {
      opt.vsync = false;
      continue;
    }
    if (std::strcmp(a, "--debug-overlay") == 0) {
      opt.debug_overlay = true;
      continue;
    }
    if (std::strcmp(a, "--bench-time") == 0 && i + 1 < argc) {
      opt.bench_hour = static_cast<float>(std::atof(argv[++i]));
      continue;
    }
    if (std::strcmp(a, "--bench-shots") == 0 && i + 1 < argc) {
      opt.bench_shots = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--bench-map") == 0 && i + 1 < argc) {
      opt.bench_map = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--bench-drive") == 0 && i + 1 < argc) {
      opt.bench_drive_seconds = static_cast<float>(std::atof(argv[++i]));
      continue;
    }
    if (std::strcmp(a, "--root") == 0 && i + 1 < argc) {
      root_arg = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--trace") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing path for --trace\n");
        return 2;
      }
      trace_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--log-all") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0]) {
        std::fprintf(stderr, "missing directory for --log-all\n");
        return 2;
      }
      log_all_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--physics-telemetry") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing path for --physics-telemetry\n");
        return 2;
      }
      physics_telemetry_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--metrics") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing path for --metrics\n");
        return 2;
      }
      diagnostics_session.metrics_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--native-report") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0]) {
        std::fprintf(stderr, "missing path for --native-report\n");
        return 2;
      }
      diagnostics_session.native_report_path = argv[++i];
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

  if (log_all_path && trace_path) {
    std::fprintf(stderr, "--trace and --log-all are mutually exclusive; log-all owns events.jsonl\n");
    return 2;
  }
  if (log_all_path && !diagnostics_session.metrics_path.empty()) {
    std::fprintf(stderr, "--metrics and --log-all are mutually exclusive; log-all owns metrics.json\n");
    return 2;
  }
  if (log_all_path) {
    crash_logging::Options options;
    options.directory = log_all_path;
    options.preserve_console_output = true;
    options.build_id = SLRR_VERSION;
    options.build_configuration = SLRR_BUILD_CONFIGURATION;
    options.native_report = !diagnostics_session.native_report_path.empty();
    std::string error;
    if (!crash_logging::start(options, error)) {
      std::fprintf(stderr, "FAIL: %s\n", error.c_str());
      return 2;
    }
    if (options.native_report) {
      diagnostics_session.native_report_path =
          crash_logging::paths()->native_report.string();
    }
  }

  if (compare_left) {
    auto comparison = physics_telemetry::compare_replay_files(
        compare_left, compare_right, physics_tolerance);
    if (!comparison.error.empty()) {
      std::fprintf(stderr, "FAIL: %s\n", comparison.error.c_str());
      return 2;
    }
    if (compare_report) {
      std::string error;
      if (!physics_telemetry::write_comparison_report(
              compare_report, comparison.report_json, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 2;
      }
    }
    if (!comparison.equal) {
      std::fprintf(stderr, "MISMATCH: %s\n", comparison.report_json.c_str());
      return 4;
    }
    std::printf("[physics] replay match %s\n", comparison.report_json.c_str());
    return 0;
  }

  if (want_physics) {
    physics_telemetry::ProbeResult result;
    std::string error;
    if (!physics_telemetry::run_deterministic_probe(
            physics_telemetry_path ? physics_telemetry_path : "",
            result, error, physics_tolerance)) {
      std::fprintf(stderr, "FAIL: %s\n", error.c_str());
      return 3;
    }
    std::printf("[physics] render partitions match: ticks=%llu; "
                "stall diverges under current lag policy: %s\n",
                static_cast<unsigned long long>(result.partition_ticks),
                result.first_lag_mismatch.c_str());
    return 0;
  }

  if (!want_probe && !want_game && !want_pacejka && !want_dyno && !want_gc) {
    usage();
    return 1;
  }

  if (physics_telemetry_path) {
    physics_telemetry::Identity identity;
    identity.scenario = want_game ? "game" : want_probe ? "boot-probe"
        : want_gc ? "gc-probe" : want_dyno ? "dyno-probe" : "pacejka-probe";
    std::string error;
    if (!physics_telemetry::start(physics_telemetry_path, identity, error)) {
      std::fprintf(stderr, "FAIL: %s\n", error.c_str());
      return 2;
    }
  }

  if (trace_path) {
    std::string error;
    if (!diagnostics::start(trace_path, error)) {
      std::fprintf(stderr, "FAIL: %s\n", error.c_str());
      return 2;
    }
  }

  if (want_pacejka) {
    char msg[256];
    const bool ok = sl2_pacejka_self_check(msg, sizeof(msg));
    std::printf("[phys] %s\n", msg);
    if (!ok) return 3;
    if (!want_probe && !want_game && !want_dyno) return 0;
  }

  if (want_dyno) {
    char msg[256];
    const bool ok = dyno_self_check(msg, sizeof(msg));
    std::printf("[phys] %s\n", msg);
    if (!ok) return 3;
    if (!want_probe && !want_game) return 0;
  }

  if (const char* env = std::getenv("SLRR_GAME_ROOT")) {
    if (!root_arg) root_arg = env;
  }

  opt.game_root = find_game_root(root_arg);
  if (opt.game_root.empty()) {
    const bool hermetic_gc = want_gc && !want_probe && !want_game &&
                             !want_pacejka && !want_dyno && !root_arg;
    if (!hermetic_gc) {
      std::fprintf(stderr,
                   "FAIL: game root not found (need system/Scripts/lang/System.class)\n"
                   "  try: --root data\\game   or set SLRR_GAME_ROOT\n");
      return 1;
    }
    std::printf("[gc] no game data; using hermetic object-graph probe\n");
  } else {
    std::printf("[host] game_root=%s\n", opt.game_root.c_str());
  }
  if (physics_telemetry::enabled())
    physics_telemetry::note_content_hash(physics_telemetry::hash_file(
        opt.game_root + "/system/Scripts/lang/System.class"));

  NativeRegistry natives;
  diagnostics_session.natives = &natives;
  Jvm jvm(&natives);
  MetricsFinalizer metrics_finalizer(diagnostics_session);
  CrashLogFinalizer crash_log_finalizer;
  NativeReportFinalizer native_report_finalizer(diagnostics_session);
  if (want_gc) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // An empty diagnostic root still initializes class loading. The probe uses
    // native Object finalization; no game classes or game-data outputs are needed.
    jvm.set_game_root(opt.game_root.empty() ? "." : opt.game_root);
    if (!jvm.bootstrap_classpath()) return 3;
    if (opt.game_root.empty()) {
      std::string error;
      if (!prepare_hermetic_gc_classes(jvm, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 3;
      }
    }
    // Isolated, headless diagnostic. Never starts Init, a game or a renderer.
    Object* host_root = nullptr;
    jvm.add_gc_roots([&](const GcRootVisitor& visit) { visit(host_root); });
    jvm.collect_garbage();
    const size_t baseline = jvm.heap_size();
    Object* a = jvm.new_object("java.lang.Object");
    Object* b = jvm.new_object("java.lang.Object");
    a->fields["peer"] = Value::make_obj(b);
    b->fields["peer"] = Value::make_obj(a);
    host_root = a;
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline + 2) return 11;
    a->gc_disabled = true;
    host_root = nullptr;
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline + 2) return 12;
    a->gc_disabled = false;
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline) return 13;
    Object* array = jvm.new_object("java.lang.Object");
    array->is_array = true;
    array->slots.push_back(Value::make_obj(jvm.new_string("gc retained string")));
    jvm.set_static("java.lang.Object", "_gcProbe", Value::make_obj(array));
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline + 2) return 14;
    jvm.set_static("java.lang.Object", "_gcProbe", Value::make_obj(nullptr));
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline) return 15;
    Object* resurrect = jvm.new_object("java.lang.Object");
    int finalized = 0;
    bool deferred = false;
    bool can_resurrect = true;
    natives.register_method("java.lang.Object", "finalize", "()V",
        [&](Object* o, const std::vector<Value>&) {
          if (can_resurrect && o == resurrect) {
            ++finalized;
            jvm.set_static("java.lang.Object", "_gcProbe", Value::make_obj(o));
            jvm.new_string("allocated by finalizer");
            deferred = jvm.collect_garbage() == -1;
          }
          return Value::make_void();
        });
    jvm.collect_garbage();
    if (finalized != 1 || !deferred || !resurrect->gc_finalized) return 16;
    jvm.set_static("java.lang.Object", "_gcProbe", Value::make_obj(nullptr));
    jvm.collect_garbage();
    if (finalized != 1 || jvm.heap_size() != baseline) return 17;
    can_resurrect = false;
    for (int i = 0; i < 20000; ++i) jvm.new_string("temporary");
    jvm.gc_safe_point();
    if (jvm.heap_size() != baseline) return 18;
    register_gc_natives(jvm);
    Object* pinned = jvm.new_object("java.lang.Object");
    natives.invoke("java.lang.Object", "disableGC", "()V", pinned, {});
    natives.invoke("java.lang.Object", "disableGC", "()V", pinned, {});
    if (!pinned->gc_disabled) return 19;
    const auto code = natives.invoke("java.lang.Object", "hashCode", "()I", pinned, {});
    if (code.tag != ValueTag::Int || !code.v.i) return 20;
    natives.invoke("java.lang.Object", "enableGC", "()V", pinned, {});
    if (pinned->gc_disabled) return 21; // Idempotent flags, not nested counters.
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline) return 22;
    Object* native_root = jvm.new_object("java.lang.Object");
    native_root->fields["payload"] = Value::make_obj(jvm.new_string("native-owned"));
    gameref_upsert(native_root);
    jvm.add_gc_roots(gameref_visit_gc_roots);
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline + 2) return 23;
    gameref_remove(native_root);
    jvm.collect_garbage();
    if (jvm.heap_size() != baseline) return 24;
    for (int i = 0; i < 16; ++i) jvm.new_string("requested cycle");
    Value pending = jvm.invoke("java.lang.System", "gc", "()I", {}, true);
    if (pending.tag != ValueTag::Int) return 25;
    if (pending.v.i >= 0) pending = jvm.invoke("java.lang.System", "gc", "()I", {}, true);
    if (pending.v.i != -1) return 26;
    jvm.gc_safe_point();
    const Value finished = jvm.invoke("java.lang.System", "gc", "()I", {}, true);
    if (finished.tag != ValueTag::Int || finished.v.i < 16) return 27;
    std::printf("[gc] probe: roots, cycles, arrays, pinning, resurrection, deferred collection and allocation pressure OK\n");
    return 0;
  }

  if (want_probe && !want_game) return boot_probe(jvm, opt);

  auto render = create_render_backend(opt.render);
  return boot_game(jvm, *render, opt);
}
