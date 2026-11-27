// #488: mod key slots in the patched ControlSet and their delivery.
// Compiles the shipped ControlSet.java and ModKeys.java with small stubs of
// the stock classes they use, then checks old controls files, the slot each
// key id keeps across boots and registration orders, overflow, and that a
// mod key's virtual axis reaches a hotkey like a stock axis.
#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/host_input.hpp"
#include <cstring>
#include <iterator>
#include <sstream>
using namespace slrr;
namespace {
constexpr int kModFirst = 118, kModSlots = 16, kControls = 134;
constexpr int kAxisBase = 0x4D4B0000;
constexpr int kG = 0x22, kH = 0x23, kJ = 0x24, kK = 0x25, kB = 0x30;

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  SLRR_REQUIRE(bool(in));
  return {std::istreambuf_iterator<char>(in), {}};
}

const char* kInput = R"JAVA(package java.io;
import java.lang.*;
public class Input {
 public final static int AXIS_NULL = 0; public final static int AXIS_TURN_LEFTRIGHT = 1; public final static int AXIS_TURN_UPDOWN = 2; public final static int AXIS_TURN_CCWCW = 3;
 public final static int AXIS_MOVE_LEFTRIGHT = 4; public final static int AXIS_MOVE_UPDOWN = 5; public final static int AXIS_MOVE_FWDBACK = 6; public final static int AXIS_LOOK_LEFTRIGHT = 7;
 public final static int AXIS_LOOK_UPDOWN = 8; public final static int AXIS_LOOK_CCWCW = 9; public final static int AXIS_LOOK_FWDBACK = 10; public final static int AXIS_LOOK_ZOOM = 11;
 public final static int AXIS_LOOK_PREVNEXT = 12; public final static int AXIS_LOOK_REAR = 13; public final static int AXIS_MENU_LEFTRIGHT = 14; public final static int AXIS_MENU_UPDOWN = 15;
 public final static int AXIS_MENU_PREVNEXT = 16; public final static int AXIS_FIRE_PRI = 17; public final static int AXIS_FIRE_SEC = 18; public final static int AXIS_USE = 19;
 public final static int AXIS_PRI_PREVNEXT = 20; public final static int AXIS_SEC_PREVNEXT = 21; public final static int AXIS_TARGET_PREVNEXT = 22; public final static int AXIS_TARGET_LEFTRIGHT = 23;
 public final static int AXIS_TARGET_UPDOWN = 24; public final static int AXIS_CONTROL_PREVNEXT = 25; public final static int AXIS_CONTROL_LEFTRIGHT = 26; public final static int AXIS_CONTROL_UPDOWN = 27;
 public final static int AXIS_THROTTLE = 28; public final static int AXIS_BRAKE = 29; public final static int AXIS_SHIFT_UPDOWN = 30; public final static int AXIS_NITRO = 31;
 public final static int AXIS_START = 32; public final static int AXIS_PAUSE = 33; public final static int AXIS_SELECT = 34; public final static int AXIS_CANCEL = 35;
 public final static int AXIS_MENU = 36; public final static int AXIS_MAP = 37; public final static int AXIS_HELP = 38; public final static int AXIS_CHEAT_1 = 39;
 public final static int AXIS_CHEAT_2 = 40; public final static int AXIS_SHIFT_LOOK = 41; public final static int AXIS_CURSOR_X = 42; public final static int AXIS_CURSOR_Y = 43;
 public final static int AXIS_CURSOR_Z = 44; public final static int AXIS_CURSOR_BUTTON1 = 45; public final static int AXIS_CURSOR_BUTTON2 = 46; public final static int AXIS_CURSOR_BUTTON3 = 47;
 public final static int AXIS_HANDBRAKE = 48; public final static int AXIS_CLUTCH = 49; public final static int AXIS_GEAR_UPDOWN = 50; public final static int AXIS_GEAR_SET = 51;
 public final static int AXIS_MUSIC_VOLUME = 52; public final static int AXIS_MUSIC_SELECT = 53; public final static int AXIS_HORN = 54; public final static int AXIS_MENU_UP = 55;
 public final static int AXIS_MENU_DOWN = 56; public final static int AXIS_MENU_LEFT = 57; public final static int AXIS_MENU_RIGHT = 58; public final static int AXIS_MUSIC_VOLUME_UP = 59;
 public final static int AXIS_MUSIC_VOLUME_DOWN = 60; public final static int AXIS_MUSIC_SELECT_NEXT = 61; public final static int AXIS_MUSIC_SELECT_PREV = 62; public final static int AXIS_PRINTSCREEN = 63;
 public final static int AXIS_SPECIAL_1 = 68; public final static int AXIS_SPECIAL_2 = 69; public final static int AXIS_SPECIAL_3 = 70; public final static int AXIS_SPECIAL_4 = 71;
 public final static int AXIS_SPECIAL_5 = 72; public final static int AXIS_SPECIAL_6 = 73; public final static int AXIS_SPECIAL_7 = 74; public final static int AXIS_SPECIAL_8 = 75;
 public final static int RCDIK_ESCAPE = 0x01; public final static int RCDIK_1 = 0x02; public final static int RCDIK_2 = 0x03; public final static int RCDIK_3 = 0x04;
 public final static int RCDIK_4 = 0x05; public final static int RCDIK_5 = 0x06; public final static int RCDIK_6 = 0x07; public final static int RCDIK_7 = 0x08;
 public final static int RCDIK_8 = 0x09; public final static int RCDIK_9 = 0x0A; public final static int RCDIK_0 = 0x0B; public final static int RCDIK_BACK = 0x0e;
 public final static int RCDIK_TAB = 0x0f; public final static int RCDIK_Q = 0x10; public final static int RCDIK_W = 0x11; public final static int RCDIK_E = 0x12;
 public final static int RCDIK_R = 0x13; public final static int RCDIK_T = 0x14; public final static int RCDIK_Y = 0x15; public final static int RCDIK_U = 0x16;
 public final static int RCDIK_I = 0x17; public final static int RCDIK_O = 0x18; public final static int RCDIK_P = 0x19; public final static int RCDIK_ENTER = 0x1c;
 public final static int RCDIK_A = 0x1e; public final static int RCDIK_S = 0x1f; public final static int RCDIK_D = 0x20; public final static int RCDIK_F = 0x21;
 public final static int RCDIK_G = 0x22; public final static int RCDIK_H = 0x23; public final static int RCDIK_J = 0x24; public final static int RCDIK_K = 0x25;
 public final static int RCDIK_L = 0x26; public final static int RCDIK_Z = 0x2c; public final static int RCDIK_X = 0x2d; public final static int RCDIK_C = 0x2e;
 public final static int RCDIK_V = 0x2f; public final static int RCDIK_B = 0x30; public final static int RCDIK_N = 0x31; public final static int RCDIK_M = 0x32;
 public final static int RCDIK_SPACE = 0x39; public final static int RCDIK_F1 = 0x3b; public final static int RCDIK_F2 = 0x3c; public final static int RCDIK_F3 = 0x3d;
 public final static int RCDIK_F4 = 0x3e; public final static int RCDIK_F5 = 0x3f; public final static int RCDIK_F6 = 0x40; public final static int RCDIK_F7 = 0x41;
 public final static int RCDIK_F8 = 0x42; public final static int RCDIK_F9 = 0x43; public final static int RCDIK_F10 = 0x44; public final static int RCDIK_F11 = 0x57;
 public final static int RCDIK_F12 = 0x58; public final static int RCDIK_UP = 0xc8; public final static int RCDIK_LEFT = 0xcb; public final static int RCDIK_RIGHT = 0xcd;
 public final static int RCDIK_DOWN = 0xd0; public final static int RCDIK_COMMA = 0x33; public final static int RCDIK_PERIOD = 0x34; public final static int RCDIK_NUMLOCK = 0x45;
 public final static int RCDIK_NUMPADSLASH = 0xb5; public final static int RCDIK_NUMPADSTAR = 0x37; public final static int RCDIK_NUMPADMINUS = 0x4a; public final static int RCDIK_NUMPADPLUS = 0x4e;
 public final static int RCDIK_NUMPADENTER = 0x9c; public final static int RCDIK_NUMPADDECIMAL = 0x53; public final static int RCDIK_NUMPAD0 = 0x52; public final static int RCDIK_NUMPAD1 = 0x4f;
 public final static int RCDIK_NUMPAD2 = 0x50; public final static int RCDIK_NUMPAD3 = 0x51; public final static int RCDIK_NUMPAD4 = 0x4b; public final static int RCDIK_NUMPAD5 = 0x4c;
 public final static int RCDIK_NUMPAD6 = 0x4d; public final static int RCDIK_NUMPAD7 = 0x47; public final static int RCDIK_NUMPAD8 = 0x48; public final static int RCDIK_NUMPAD9 = 0x49;
 public final static int RCDIK_PGUP = 0xc9; public final static int RCDIK_PGDN = 0xd1;
 public native static String getDeviceName( int i );
}
)JAVA";

const char* kStubs[][2] = {
  {"String.java", "package java.lang; public class String { public native int length(); }"},
  {"System.java", "package java.lang; public class System { public static native void log( String s ); }"},
  {"Config.java", R"JAVA(package java.util;
public class Config {
 static float throttleSpeed; static float throttlePower; static float brakeSpeed; static float brakePower;
 static float steerSpeed; static float steeringPower; static float clutchPower;
 static float deadZoneAccelerate; static float deadZoneBrake; static float deadZoneSteering; static float deadZoneClutch;
})JAVA"},
  {"Vector.java", R"JAVA(package java.util;
public class Vector {
 Object[] data; int count;
 public Vector() { data = new Object[16]; }
 public Vector( int n ) { data = new Object[16]; }
 public void addElement( Object o ) { data[count] = o; count++; }
 public Object elementAt( int i ) { return data[i]; }
 public int size() { return count; }
})JAVA"},
  {"File.java", R"JAVA(package java.io;
import java.lang.*;
public class File {
 final static int MODE_READ = 0; final static int MODE_WRITE = 1;
 String name;
 public File( String n ) { name = n; }
 public native int open( int mode ); public native void close();
 public native int write( int value ); public native int write( float value ); public native int write( String value );
 public native int readInt(); public native float readFloat(); public native String readString();
})JAVA"},
  {"Controller.java", "package java.io; import java.lang.*; public class Controller { static String controlFile; }"},
  {"InputDevices.java", R"JAVA(package java.io;
public class InputDevices {
 public final static int KIND_KEYBOARD = 0; public final static int KIND_MOUSE = 1;
 public final static int KIND_PAD = 2; public final static int KIND_WHEEL = 3;
 public static native int kind( int device );
})JAVA"},
  {"Handler.java", "package probe; public class Handler { public native void osdCommand( int command ); }"},
};

// One boot: a fresh VM with the patched ControlSet and ModKeys.
struct Boot {
  Fixture f;
  Boot() {
    register_script_natives(f.vm, f.packs, f.sections);
    const fs::path src = f.root / "probe/src";
    fs::create_directories(src);
    std::ofstream(src / "Input.java") << kInput;
    for (const auto& stub : kStubs) std::ofstream(src / stub[0]) << stub[1];
    std::ofstream(src / "ControlSet.java") << read_text("patch/system/Scripts/io/src/ControlSet.java");
    std::ofstream(src / "ModKeys.java") << read_text("patch/system/Scripts/openslrr/src/ModKeys.java");
    const auto report = script_compile_all(f.root.string(), "probe");
    SLRR_REQUIRE(report.failed == 0 && report.succeeded > 0);
    for (const auto& entry : fs::directory_iterator(f.root / "probe"))
      if (entry.path().extension() == ".class") SLRR_REQUIRE(f.vm.load_class_file(entry.path().string()));
    fs::create_directories(f.root / "save/controls");
  }
  Value str(const std::string& s) { return Value::make_obj(f.vm.new_string(s)); }
  int reg(const char* id, const char* label, int key) {
    return f.vm.invoke("java.openslrr.ModKeys", "register", "(Ljava.lang.String;Ljava.lang.String;I)I",
                       {id ? str(id) : Value::make_obj(nullptr), str(label), Value::make_int(key)}, true).v.i;
  }
  int keys(const char* method, int i) {
    return f.vm.invoke("java.openslrr.ModKeys", method, "(I)I", {Value::make_int(i)}, true).v.i;
  }
  int count() { return f.vm.invoke("java.openslrr.ModKeys", "count", "()I", {}, true).v.i; }
  Object* set() {
    Object* cs = f.vm.new_object("java.io.ControlSet");
    SLRR_REQUIRE(cs);
    f.vm.invoke("java.io.ControlSet", "<init>", "()V", {Value::make_obj(cs)}, false);
    return cs;
  }
  void call(Object* cs, const char* method, const std::string& file) {
    f.vm.invoke("java.io.ControlSet", method, "(Ljava.lang.String;)V", {Value::make_obj(cs), str(file)}, false);
  }
  Object* loaded(const std::string& file) { Object* cs = set(); call(cs, "load", file); return cs; }
  int slot_of(Object* cs, int i) {
    return f.vm.invoke("java.io.ControlSet", "modSlot", "(I)I", {Value::make_obj(cs), Value::make_int(i)}, false).v.i;
  }
  static Value& at(Object* cs, const char* field, int i) {
    Value& array = cs->fields[field];
    SLRR_REQUIRE(array.tag == ValueTag::Obj && array.v.o && i < int(array.v.o->slots.size()));
    return array.v.o->slots[size_t(i)];
  }
  static int get(Object* cs, const char* field, int i) {
    const Value& v = at(cs, field, i);
    return v.tag == ValueTag::Int ? v.v.i : v.tag == ValueTag::Float ? int(v.v.f) : 0;
  }
  void change(Object* cs, int slot, int device, int axis) {
    f.vm.invoke("java.io.ControlSet", "change", "(III)V",
                {Value::make_obj(cs), Value::make_int(slot), Value::make_int(device), Value::make_int(axis)}, false);
  }
  void copy_to(Boot& other, const std::string& file) {
    fs::copy_file(f.root / file, other.f.root / file, fs::copy_options::overwrite_existing);
  }
};

// A controls file as a stock or #405 build saved it: version 16 with n
// controls, every slot a distinct key, no mod key trailer.
void write_old_file(const fs::path& path, int n) {
  std::ofstream out(path, std::ios::binary);
  auto word = [&](int32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto real = [&](float v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto text = [&](const char* s) { word(int32_t(std::strlen(s) + 1)); out.write(s, std::streamsize(std::strlen(s) + 1)); };
  word(0x4c525443); word(16); word(2); text("SysKeyboard"); text("SysMouse");
  word(n);
  for (int i = 0; i < n; ++i) {
    word(1); word(28); word(0); word(1 + i % 200); real(0); real(1); real(0); real(1); real(0);
  }
  word(0);
  SLRR_REQUIRE(bool(out));
}

void old_files_keep_loading() {
  for (int n : {78, 118}) {
    Boot boot;
    SLRR_REQUIRE(boot.reg("a.view", "A VIEW", kG) == kAxisBase);
    write_old_file(boot.f.root / "save/controls/old", n);
    Object* cs = boot.loaded("save/controls/old");
    for (int i : {0, 5, 40, 77}) SLRR_REQUIRE(Boot::get(cs, "axisID", i) == 1 + i % 200);
    if (n == 118) SLRR_REQUIRE(Boot::get(cs, "axisID", 117) == 1 + 117 % 200);
    else SLRR_REQUIRE(Boot::get(cs, "deviceID", 100) == -1);  // pad and wheel defaults
    // The key takes the first mod slot with its default key, in DEFAULTSET.
    SLRR_REQUIRE(boot.slot_of(cs, 0) == kModFirst && boot.keys("control", 0) == kModFirst);
    SLRR_REQUIRE(Boot::get(cs, "deviceID", kModFirst) == 0 && Boot::get(cs, "axisID", kModFirst) == kG);
    SLRR_REQUIRE(Boot::get(cs, "vaxisID", kModFirst) == kAxisBase && Boot::get(cs, "group", kModFirst) == 0);
    for (int s = kModFirst + 1; s < kControls; ++s)
      SLRR_REQUIRE(Boot::get(cs, "deviceID", s) == -1 && Boot::get(cs, "vaxisID", s) == 0);
  }
  std::puts("PASS 78 and 118 slot files load; mod slots keep their defaults");
}

void binding_follows_id() {
  const std::string file = "save/controls/mk";
  Boot one;
  SLRR_REQUIRE(one.reg("a.x", "A", kG) == kAxisBase);
  SLRR_REQUIRE(one.reg("b.y", "B", kH) == kAxisBase + 1);
  write_old_file(one.f.root / file, 118);
  Object* cs = one.loaded(file);
  const int b_slot = one.slot_of(cs, 1);
  SLRR_REQUIRE(b_slot == kModFirst + 1);
  one.change(cs, b_slot, 0, kB);
  one.call(cs, "save", file);
  // The saved file has 134 slots and loads back as it was.
  Object* back = one.loaded(file);
  SLRR_REQUIRE(Boot::get(back, "axisID", b_slot) == kB && Boot::get(back, "axisID", 5) == 6);

  // Next boot: another order and a new key first.
  Boot two;
  one.copy_to(two, file);
  SLRR_REQUIRE(two.reg("c.z", "C", kJ) == kAxisBase);
  SLRR_REQUIRE(two.reg("b.y", "B", kH) == kAxisBase + 1);
  SLRR_REQUIRE(two.reg("a.x", "A", kG) == kAxisBase + 2);
  cs = two.loaded(file);
  SLRR_REQUIRE(two.slot_of(cs, 1) == b_slot && Boot::get(cs, "axisID", b_slot) == kB);
  SLRR_REQUIRE(Boot::get(cs, "vaxisID", b_slot) == kAxisBase + 1);
  SLRR_REQUIRE(two.slot_of(cs, 2) == kModFirst && Boot::get(cs, "axisID", kModFirst) == kG);
  SLRR_REQUIRE(Boot::get(cs, "vaxisID", kModFirst) == kAxisBase + 2);
  // The new key takes a slot no key used, with its default.
  SLRR_REQUIRE(two.slot_of(cs, 0) == kModFirst + 2 && Boot::get(cs, "axisID", kModFirst + 2) == kJ);
  for (int i = 0; i < 3; ++i) SLRR_REQUIRE(two.keys("control", i) == two.slot_of(cs, i));
  // The player clears A: its default is not put back on the next load.
  two.change(cs, kModFirst, -1, -1);
  two.call(cs, "save", file);
  cs = two.loaded(file);
  // (An unbound slot reads back as device 0, axis -1, as in the stock loader.)
  SLRR_REQUIRE(Boot::get(cs, "axisID", kModFirst) == -1);

  // A boot without b.y: its slot keeps the binding but drives no axis; a new
  // key does not take it while a never-used slot is free.
  Boot three;
  two.copy_to(three, file);
  SLRR_REQUIRE(three.reg("d.w", "D", kK) == kAxisBase);
  cs = three.loaded(file);
  SLRR_REQUIRE(Boot::get(cs, "vaxisID", b_slot) == 0 && Boot::get(cs, "axisID", b_slot) == kB);
  SLRR_REQUIRE(three.slot_of(cs, 0) == kModFirst + 3 && Boot::get(cs, "axisID", kModFirst + 3) == kK);
  three.call(cs, "save", file);

  // b.y comes back and finds its binding.
  Boot four;
  three.copy_to(four, file);
  SLRR_REQUIRE(four.reg("b.y", "B", kH) == kAxisBase);
  cs = four.loaded(file);
  SLRR_REQUIRE(four.slot_of(cs, 0) == b_slot && Boot::get(cs, "axisID", b_slot) == kB);
  SLRR_REQUIRE(Boot::get(cs, "vaxisID", b_slot) == kAxisBase);
  std::puts("PASS a binding follows its key id across boots, orders, clearing and uninstall");
}

// A cut MKEY trailer (a file truncated by hand or an interrupted copy) loads
// what is complete and never reads past the end. Binary files, as an older
// build or the stock game writes them.
void truncated_trailer_is_safe() {
  const std::string file = "save/controls/cut";
  // 4: only the id; 8: id and count; 10, 22: inside the first record; 24: the
  // first record whole; 34: inside the second.
  for (const int keep : {-1, 4, 8, 10, 22, 24, 34}) {
    Boot boot;
    SLRR_REQUIRE(boot.reg("a.x", "A", kG) == kAxisBase && boot.reg("b.y", "B", kH) == kAxisBase + 1);
    const auto path = boot.f.root / file;
    write_old_file(path, kControls);
    std::string trailer;
    auto word = [&](int32_t v) { trailer.append(reinterpret_cast<const char*>(&v), 4); };
    auto text = [&](const char* id) { word(int32_t(std::strlen(id) + 1)); trailer.append(id, std::strlen(id) + 1); };
    word(0x59454b4d); word(2);
    word(kModFirst + 1); text("a.x"); word(0);   // a.x owns slot 119
    word(kModFirst); text("b.y"); word(0);       // b.y owns slot 118
    if (keep >= 0) trailer.resize(size_t(keep));
    { std::ofstream out(path, std::ios::binary | std::ios::app); out << trailer; }
    Object* cs = boot.loaded(file);
    // The slot table before the trailer is intact.
    SLRR_REQUIRE(Boot::get(cs, "axisID", 5) == 6 && Boot::get(cs, "axisID", 100) == 101);
    const int a = boot.slot_of(cs, 0), b = boot.slot_of(cs, 1);
    SLRR_REQUIRE(a >= kModFirst && b >= kModFirst && a != b);
    if (keep < 0) SLRR_REQUIRE(a == kModFirst + 1 && b == kModFirst);      // whole trailer
    else if (keep >= 24) SLRR_REQUIRE(a == kModFirst + 1 && b != kModFirst + 1);  // first record only
    else SLRR_REQUIRE(a == kModFirst && b == kModFirst + 1);               // none: new keys in order
  }
  std::puts("PASS a truncated mod key trailer loads its complete records and stops at the end");
}

// A clear is the player's action, recorded when it happens: a key without a
// default, bound and then cleared, stays cleared when a later version of the
// mod gives it a default; binding it again lifts the clear.
void clear_survives_a_new_default() {
  const std::string file = "save/controls/clr";
  Boot one;
  SLRR_REQUIRE(one.reg("m.key", "M", 0) == kAxisBase);
  Object* cs = one.set();
  const int slot = one.slot_of(cs, 0);
  one.change(cs, slot, 0, kH);
  one.change(cs, slot, -1, -1);  // Esc in the options
  one.call(cs, "save", file);
  Boot two;
  one.copy_to(two, file);
  SLRR_REQUIRE(two.reg("m.key", "M", kK) == kAxisBase);
  Object* back = two.loaded(file);
  SLRR_REQUIRE(two.slot_of(back, 0) == slot && Boot::get(back, "axisID", slot) == -1);
  // Bound again and saved: the clear is gone, the binding stays.
  two.change(back, slot, 0, kJ);
  two.call(back, "save", file);
  Boot three;
  two.copy_to(three, file);
  SLRR_REQUIRE(three.reg("m.key", "M", kK) == kAxisBase);
  Object* again = three.loaded(file);
  SLRR_REQUIRE(Boot::get(again, "axisID", slot) == kJ);
  // Unbound by something other than the player (a file without the key's
  // binding) still takes the default: only change() records a clear.
  std::puts("PASS a cleared mod key stays cleared under a new default; rebinding lifts it");
}

void registration_limits() {
  Boot boot;
  SLRR_REQUIRE(boot.reg(nullptr, "none", kG) == -1 && boot.reg("", "empty", kG) == -1);
  for (int i = 0; i < kModSlots; ++i) {
    const std::string id = "m.k" + std::to_string(i);
    SLRR_REQUIRE(boot.reg(id.c_str(), "K", 0) == kAxisBase + i);
  }
  SLRR_REQUIRE(boot.reg("m.extra", "EXTRA", kG) == -1 && boot.count() == kModSlots);
  SLRR_REQUIRE(boot.reg("m.k3", "AGAIN", kG) == kAxisBase + 3 && boot.count() == kModSlots);
  // Every key gets its own slot, also over a file whose slots belong to
  // keys no mod registers now.
  const std::string file = "save/controls/full";
  {
    Boot other;
    for (int i = 0; i < kModSlots; ++i) other.reg(("gone.k" + std::to_string(i)).c_str(), "G", kG);
    Object* cs = other.set();
    other.call(cs, "save", file);
    other.copy_to(boot, file);
  }
  Object* cs = boot.loaded(file);
  std::vector<bool> used(kModSlots);
  for (int i = 0; i < kModSlots; ++i) {
    const int slot = boot.slot_of(cs, i);
    SLRR_REQUIRE(slot >= kModFirst && slot < kControls && !used[size_t(slot - kModFirst)]);
    used[size_t(slot - kModFirst)] = true;
    SLRR_REQUIRE(Boot::get(cs, "vaxisID", slot) == kAxisBase + i && Boot::get(cs, "deviceID", slot) == -1);
  }
  std::puts("PASS empty ids and a 17th key are refused; re-registering returns the same axis");
}

// The engine side: a mod key's virtual axis, bound like Controller.add binds
// a ControlSet slot, drives a VIRTUAL hotkey and user_GetAxisVal untruncated.
void virtual_axis_delivery() {
  Boot boot;
  SLRR_REQUIRE(boot.reg("a.x", "A", kG) == kAxisBase);
  Object* cs = boot.set();
  const int slot = boot.keys("control", 0);
  auto* handler = boot.f.vm.new_object("probe.Handler");
  int calls = 0, last = 0;
  boot.f.registry.register_method("probe.Handler", "osdCommand", "(I)V", [&](Object*, const std::vector<Value>& a) {
    ++calls; last = a.empty() ? 0 : a[0].v.i; return Value::make_void(); });
  Object controller, hotkey, other;
  controller.input = std::make_unique<ControllerInput>();
  auto I = [](int v) { return Value::make_int(v); };
  auto F = [](float v) { return Value::make_float(v); };
  const int vaxis = Boot::get(cs, "vaxisID", slot);
  SLRR_REQUIRE(vaxis == kAxisBase);
  SLRR_REQUIRE(boot.f.registry.invoke("java.io.Controller", "user_Add", "(IIIFFFF)I", &controller,
      {I(vaxis), I(Boot::get(cs, "deviceID", slot)), I(Boot::get(cs, "axisID", slot)), F(0), F(1), F(0), F(1)}).v.i == 1);
  hotkey.fields["command"] = I(77);
  other.fields["command"] = I(78);
  host_input_clear_keys();
  // Osd.createHotkey passes Input.VIRTUAL with HK_STATIC removed; the host
  // reads only the VIRTUAL bit either way.
  host_input_create_hotkey(boot.f.vm, &hotkey, handler, nullptr, vaxis, 1, 1);
  host_input_create_hotkey(boot.f.vm, &other, handler, nullptr, vaxis + 1, 1 | 4, 1);
  host_input_check_hotkeys(boot.f.vm, &controller, nullptr);
  SLRR_REQUIRE(calls == 0);
  host_input_key(kG, true);
  SLRR_REQUIRE(boot.f.registry.invoke("java.io.Controller", "user_GetAxisVal", "(I)F", &controller, {I(vaxis)}).v.f == 1.f);
  host_input_check_hotkeys(boot.f.vm, &controller, nullptr);
  SLRR_REQUIRE(calls == 1 && last == 77);  // the neighbouring axis did not fire
  host_input_key(kG, false);
  host_input_check_hotkeys(boot.f.vm, &controller, nullptr);
  SLRR_REQUIRE(calls == 1);
  host_input_forget(handler);
  host_input_clear_keys();
  std::puts("PASS a mod key's virtual axis drives a VIRTUAL hotkey and the controller axis");
}
}  // namespace

int main() {
  const standalone::Test tests[] = {old_files_keep_loading, binding_follows_id, truncated_trailer_is_safe,
                                    clear_survives_a_new_default, registration_limits,
                                    virtual_axis_delivery};
  return standalone::run("mod_keys", tests);
}
