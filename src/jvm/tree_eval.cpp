#include "slrr/tree_eval.hpp"
#include "slrr/jvm.hpp"
#include "slrr/script_threads.hpp"

#include "slrr/object.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/rpk_index.hpp"

#include <algorithm>
#include <optional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {
namespace {

constexpr uint32_t kNativeFlag = 0x40;
constexpr uint32_t kStaticFlag = 0x08;

float bits_to_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// Script float text: three decimals, trailing zeros removed. 5.5 stays 5.5,
// 3.727 stays 3.727, 13.033 stays 13.033.
std::string format_script_number(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  std::string s(buf);
  const auto dot = s.find('.');
  if (dot == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

bool values_equal(const Value& a, const Value& b) {
  if (a.tag != b.tag) {
    if ((a.tag == ValueTag::Int || a.tag == ValueTag::Float) &&
        (b.tag == ValueTag::Int || b.tag == ValueTag::Float)) {
      const float af =
          a.tag == ValueTag::Float ? a.v.f : static_cast<float>(a.v.i);
      const float bf =
          b.tag == ValueTag::Float ? b.v.f : static_cast<float>(b.v.i);
      return af == bf;
    }
    return false;
  }
  switch (a.tag) {
    case ValueTag::Void:
      return true;
    case ValueTag::Int:
      return a.v.i == b.v.i;
    case ValueTag::Float:
      return a.v.f == b.v.f;
    case ValueTag::Obj:
      if (a.v.o == b.v.o) return true;
      if (!a.v.o || !b.v.o) return false;
      // TextBox breaks a line on `token == "\n"`. The token is a new String,
      // so pointer identity never matches the literal.
      if (a.v.o->class_name == "java.lang.String" &&
          b.v.o->class_name == "java.lang.String")
        return a.v.o->str == b.v.o->str;
      return false;
  }
  return false;
}

bool truthy(const Value& v) {
  switch (v.tag) {
    case ValueTag::Void:
      return false;
    case ValueTag::Int:
      return v.v.i != 0;
    case ValueTag::Float:
      return v.v.f != 0.f;
    case ValueTag::Obj:
      // A String is an object. "" is true: FileRequester builds each row
      // with addItem("") and only then changeLabelText. Treating "" as
      // false skipped the label, so the career name never appeared.
      return v.v.o != nullptr;
  }
  return false;
}

std::string strip_type_desc(std::string t) {
  if (!t.empty() && t[0] == 'L' && t.back() == ';')
    t = t.substr(1, t.size() - 2);
  for (char& c : t)
    if (c == '/') c = '.';
  return t;
}

}  // namespace

TreeEvalResult tree_eval(TreeHost* host, const TufaClass& cls,
                         const TufaMember& method, Object* self,
                         const std::vector<Value>& args) {
  TreeEvalResult out;
  if (!host) {
    out.error = "null TreeHost";
    return out;
  }
  const std::string mname = cls.utf8(method.name);
  const std::string mdesc = cls.utf8(method.descriptor);

  if (method.flags & kNativeFlag) {
    out.error = "native method has no TREE body";
    return out;
  }
  if (method.tree == kTufaNone || method.tree >= cls.trees.size()) {
    out.error = "bad tree index";
    return out;
  }
  const auto& tree = cls.trees[method.tree];
  if (tree.empty()) {
    out.error = "empty TREE (host native expected)";
    return out;
  }
  std::vector<Value> locals;
  if (method.flags & kStaticFlag) {
    // Last parameter is local 0. File.delete(path, mask) was reading the
    // mask as the path, so FindFile looked for "*save/carrer/..." .
    for (auto it = args.rbegin(); it != args.rend(); ++it)
      locals.push_back(*it);
  } else {
    // Last parameter is local 1. `create(parent, rid, alias)` reads parent
    // from the highest slot.
    locals.push_back(Value::make_obj(self));
    for (auto it = args.rbegin(); it != args.rend(); ++it)
      locals.push_back(*it);
  }
  if (method.locals > locals.size())
    locals.resize(method.locals, Value::make_int(0));

  std::vector<Value> stack;
  std::vector<std::string> local_types(locals.size());
  auto local_value = [&](size_t slot, Value v) {
    if (slot < local_types.size()) {
      const std::string& t = local_types[slot];
      // `int price = getPrice() * PRICERATIO` — the label prints the local,
      // and the catalog shows whole dollars.
      if ((t == "I" || t == "B" || t == "S" || t == "C" || t == "Z") &&
          v.tag == ValueTag::Float) {
        int32_t n = static_cast<int32_t>(v.v.f);
        if (t == "Z") n = n != 0 ? 1 : 0;
        return Value::make_int(n);
      }
      if (t == "F" && v.tag == ValueTag::Int)
        return Value::make_float(static_cast<float>(v.v.i));
    }
    return v;
  };
  int pending_expr = -1;  // TAGS index when set via 0x07
  std::string pending_class;
  std::string pending_new_class;  // type for `new` — not clobbered by getstatic class
  // Nested `new Thread(new HotkeyWatcher(), name)` : inner new must not drop
  // the outer type.
  std::vector<std::string> new_types;
  std::string pending_name;
  std::string pending_field;
  std::string pending_field_desc;
  // `this.pic = pic` (op 0x1c + assign): receiver is on top of the value.
  Object* pending_put_recv = nullptr;
  bool have_put_recv = false;
  // A path such as `local.field = value` also leaves its explicit receiver
  // above the value, even when the field is declared by the current class.
  bool put_recv_from_path = false;
  // `elementData[i] = v` — 0x20 then assign.
  Object* pending_arr = nullptr;
  int32_t pending_arr_idx = -1;
  // `creditsPos[j] -= move` loads the slot (0x20) then a compound assign.
  // The result has to go back into that slot or the scroller never moves.
  Object* last_arr = nullptr;
  int32_t last_arr_idx = -1;
  // `elementCount++` (expr 23) writes back the field just read.
  Object* last_ifield_recv = nullptr;
  std::string last_ifield_name;
  bool last_ifield_fresh = false;
  std::string pending_method_owner;
  std::string pending_method_name;
  std::string pending_method_desc;
  int pending_argc = -1;
  int pending_local_store = -1;  // `local N; stmt assign` target
  // `int x = expr` is declare, type, init marker, expr, store. The store
  // node's imm is not the slot.
  int pending_init_local = -1;
  bool pending_call = false;
  bool call_pushed = false;
  // Unqualified `create(...)` keeps the arguments on the stack; the receiver
  // is this. `osd.endGroup()` and `new X().display()` push the receiver.
  bool call_recv_on_stack = false;
  // `groups.addElement` / `text.changeColor`: the path after CALL pushes the
  // receiver. Capturing it here stops finish_call from using `this` (the Osd)
  // and looking the method up on Osd, which left every options page visible.
  bool have_call_recv = false;
  Value call_recv_value;
  bool path_base_on_stack = false;
  bool pending_super = false;
  bool path_static = false;
  bool saw_class = false;  // path+class… → getstatic / static call
  // Last java.render.Osd the script built or called. createHotkey often
  // leaves `this` on top of the stack, so the dialog is not the viewport.
  Object* last_osd = nullptr;
  // Garage OSD: Menu receiver often buried under args — track last createMenu.
  Object* last_menu = nullptr;
  auto* vm=dynamic_cast<Jvm*>(host);
  std::optional<Jvm::RootScope> frame_roots;
  if(vm && script_thread_active(*vm))frame_roots.emplace(*vm,[&](const GcRootVisitor& visit){
    auto value=[&](const Value& v){if(v.tag==ValueTag::Obj)visit(v.v.o);};
    visit(self);for(const auto& v:args)value(v);
    for(const auto& v:locals)value(v);for(const auto& v:stack)value(v);
    visit(pending_put_recv);visit(pending_arr);visit(last_arr);visit(last_ifield_recv);
    value(call_recv_value);visit(last_osd);visit(last_menu);
  });

  auto fail = [&](const std::string& msg) -> TreeEvalResult {
    out.ok = false;
    out.error = cls.name() + "." + mname + mdesc + ": " + msg;
    return out;
  };
  auto pop = [&]() -> Value {
    if (stack.empty()) return Value::make_int(0);
    Value v = stack.back();
    stack.pop_back();
    return v;
  };
  auto push = [&](Value v) { stack.push_back(v); };

  auto do_eq = [&]() {
    if (stack.size() < 2) return;
    const Value b = pop();
    const Value a = pop();
    push(Value::make_int(values_equal(a, b) ? 1 : 0));
    pending_expr = -1;
  };

  auto do_binop = [&](int tag) {
    if (stack.size() < 2) return;
    Value b = pop();
    Value a = pop();
    // `OKButtonText + ";Cancel"` — FileRequester's confirm button. Numeric
    // add turned that into 0, so Dialog.show built no buttons.
    if (tag == 16) {
      auto is_jstring = [](const Value& v) {
        if (v.tag != ValueTag::Obj || !v.v.o) return false;
        const std::string& n = v.v.o->class_name;
        return n == "java.lang.String" || n == "String";
      };
      if (is_jstring(a) || is_jstring(b)) {
        // A null reference concatenates as empty. Mechanic builds its
        // flags with `flags = flags + "+"`, and flags starts null.
        auto piece = [](const Value& v) -> std::string {
          if (v.tag == ValueTag::Obj) return v.v.o ? v.v.o->str : "";
          if (v.tag == ValueTag::Int) return std::to_string(v.v.i);
          if (v.tag == ValueTag::Float)
            return format_script_number(static_cast<double>(v.v.f));
          return "";
        };
        push(Value::make_obj(host->tree_new_string(piece(a) + piece(b))));
        pending_expr = -1;
        return;
      }
    }
    const bool use_f =
        a.tag == ValueTag::Float || b.tag == ValueTag::Float;
    // Ordered compares on floats. `==` and `!=` compare by value below.
    if (use_f && tag >= 8 && tag <= 11) {
      auto num = [](const Value& v) -> float {
        if (v.tag == ValueTag::Float) return v.v.f;
        if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
        return 0.f;
      };
      const float af = num(a);
      const float bf = num(b);
      const bool r = tag == 8    ? af >= bf
                     : tag == 9  ? af <= bf
                     : tag == 10 ? af > bf
                                 : af < bf;
      push(Value::make_int(r ? 1 : 0));
      pending_expr = -1;
      return;
    }
    if (tag >= 15 && tag <= 19) {
      auto num = [](const Value& v) {
        if (v.tag == ValueTag::Float) return v.v.f;
        if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
        return 0.f;
      };
      const float af = num(a);
      const float bf = num(b);
      float rf = 0.f;
      if (tag == 15)
        rf = af - bf;
      else if (tag == 16)
        rf = af + bf;
      else if (tag == 17)
        rf = bf != 0.f ? std::fmod(af, bf) : 0.f;
      else if (tag == 18)
        rf = bf != 0.f ? af / bf : 0.f;
      else
        rf = af * bf;
      if (use_f)
        push(Value::make_float(rf));
      else
        push(Value::make_int(static_cast<int32_t>(rf)));
      pending_expr = -1;
      return;
    }
    int32_t ai = a.tag == ValueTag::Int ? a.v.i : 0;
    int32_t bi = b.tag == ValueTag::Int ? b.v.i : 0;
    int32_t r = 0;
    switch (tag) {
      case 3:
        r = ai | bi;
        break;
      case 4:
        r = ai ^ bi;
        break;
      case 12:
        r = static_cast<int32_t>(static_cast<uint32_t>(ai) << (static_cast<uint32_t>(bi) & 31u));
        break;
      case 13:
        r = ai >> (static_cast<uint32_t>(bi) & 31u);
        break;
      case 14:
        r = static_cast<int32_t>(static_cast<uint32_t>(ai) >> (static_cast<uint32_t>(bi) & 31u));
        break;
      case 5:
        r = ai & bi;
        break;
      case 6:
        // `!=` is the negation of `==`. Object != is pointer identity —
        // `texture != Osd.RRT_NONE` was always false, so RectangleTemplate
        // never swapped the plate texture. A float operand compares by value:
        // reading both floats as integer 0 made every float `!=` false, so
        // Tyre.SetInflation(I) never applied a new pressure and every tyre
        // stayed at its 0.5 bar floor. The MWM key check
        // (`Check(key) != Math.sqrt(key)+3.75`) computes the same float on
        // both sides and is still false.
        if (a.tag == ValueTag::Obj || b.tag == ValueTag::Obj || use_f)
          r = values_equal(a, b) ? 0 : 1;
        else
          r = ai != bi;
        break;
      case 7:
        r = values_equal(a, b) ? 1 : 0;
        break;
      case 8:
        r = ai >= bi;
        break;
      case 9:
        r = ai <= bi;
        break;
      case 10:
        r = ai > bi;
        break;
      case 11:
        r = ai < bi;
        break;
      case 15:
        r = ai - bi;
        break;
      case 16:
        r = ai + bi;
        break;
      case 19:
        r = ai * bi;
        break;
      default:
        r = 0;
        break;
    }
    push(Value::make_int(r));
    pending_expr = -1;
  };

  // `x += step` / `priority += 2`. lhs is the value just read, rhs was pushed
  // before it. Either side may be an int literal (`x = -1`).
  auto combine_assign = [](int tag, const Value& lhs, const Value& rhs) -> Value {
    auto as_i = [](const Value& v) -> int32_t {
      if (v.tag == ValueTag::Int) return v.v.i;
      if (v.tag == ValueTag::Float) return static_cast<int32_t>(v.v.f);
      return 0;
    };
    auto as_f = [](const Value& v) -> float {
      if (v.tag == ValueTag::Float) return v.v.f;
      if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
      return 0.f;
    };
    if (tag >= 44 && tag <= 46) {
      const int32_t a = as_i(lhs);
      const int32_t b = as_i(rhs);
      int32_t r = 0;
      if (tag == 44) r = a & b;
      else if (tag == 45) r = a ^ b;
      else r = a | b;
      return Value::make_int(r);
    }
    const bool use_f =
        lhs.tag == ValueTag::Float || rhs.tag == ValueTag::Float;
    const float a = as_f(lhs);
    const float b = as_f(rhs);
    float r = 0.f;
    if (tag == 36) r = a * b;
    else if (tag == 37) r = b != 0.f ? a / b : 0.f;
    else if (tag == 38) r = b != 0.f ? std::fmod(a, b) : 0.f;
    else if (tag == 39) r = a + b;
    else if (tag == 40) r = a - b;
    if (use_f) return Value::make_float(r);
    return Value::make_int(static_cast<int32_t>(r));
  };

  auto push_instance_field = [&](Object* recv, const std::string& fname,
                                 const std::string& fdesc) {
    last_ifield_recv = recv;
    last_ifield_name = fname;
    last_ifield_fresh = recv != nullptr;
    if (!recv) {
      if (!fdesc.empty() && fdesc[0] == 'L')
        push(Value::make_obj(nullptr));
      else if (fdesc == "F")
        push(Value::make_float(0.f));
      else
        push(Value::make_int(0));
      return;
    }
    auto it = recv->fields.find(fname);
    if (it == recv->fields.end()) {
      if (!fdesc.empty() && fdesc[0] == 'L')
        push(Value::make_obj(nullptr));
      else if (fdesc == "F")
        push(Value::make_float(0.f));
      else
        push(Value::make_int(0));
    } else {
      push(it->second);
    }
  };

  auto finish_call = [&]() -> TreeEvalResult {
    call_pushed = false;
    if (!pending_call) return {};
    pending_call = false;
    // call_on puts argc on the stack under the receiver, not next to the
    // opcode, so pending_argc stays unset until that receiver is popped.
    const bool argc_unset = pending_argc < 0;
    int argc = argc_unset ? 0 : pending_argc;
    pending_argc = -1;

    std::string owner = pending_method_owner;
    std::string name = pending_method_name;
    std::string desc = pending_method_desc;
    pending_method_owner.clear();
    pending_method_name.clear();
    pending_method_desc.clear();
    if (name.empty()) return fail("call without method name");

    // Stack layout: [receiver?, arg0…argN] with argN on top. Pop args first,
    // then optional instance receiver. Menu.addItem needs last_menu fallback
    // (slrr-reverse tree_eval_part3.inc).
    Object* recv = nullptr;
    // path+class before the name is a static call, including one on the
    // current class (Sound.enableDebugDump). An unqualified static such as
    // changeActiveSection(new SplashScreen) has no class prefix; the new
    // object must stay an argument, not become the receiver.
    bool own_static_method = false;
    if (!pending_super && (owner.empty() || owner == cls.name())) {
      bool in_static = false;
      bool in_instance = false;
      for (const auto& sm : cls.static_methods)
        if (cls.utf8(sm.name) == name) in_static = true;
      for (const auto& im : cls.instance_methods)
        if (cls.utf8(im.name) == name) in_instance = true;
      own_static_method = in_static && !in_instance;
    }
    Object* receiver_hint = nullptr;
    if (have_call_recv && call_recv_value.tag == ValueTag::Obj)
      receiver_hint = call_recv_value.v.o;
    else if (call_recv_on_stack && !stack.empty() && stack.back().tag == ValueTag::Obj)
      receiver_hint = stack.back().v.o;
    const bool menu_receiver = !receiver_hint ||
        receiver_hint->class_name == "java.render.osd.Menu";
    const bool menu_call =
        (name == "addItem" && menu_receiver && argc != 6 &&
         (argc_unset || argc >= 2)) || name == "addSeparator" || name == "setSliderStyle";
    const bool osd_build = name == "createMenu" || name == "createButton" ||
                           name == "createText" || name == "createRectangle" ||
                           name == "createHeader" ||
                           (name == "createHotkey" && owner != "java.io.Input") ||
                           name == "createBG" || name == "createTextBox" ||
                           name == "createStringInput" || name == "endGroup" ||
                           name == "hideGroup" || name == "showGroup" ||
                           name == "beginGroup";
    // osd.createX is an instance call. A leftover class opcode must not
    // turn it into a static lookup on the dialog class (that misses).
    // call_on is an instance call even when this class has a static method
    // of the same name. GameLogic.save(String) was stealing botVd.save(File).
    const bool is_static =
        !osd_build && !menu_call && !call_recv_on_stack &&
        (own_static_method || (saw_class && !owner.empty() && !pending_super));

    auto class_has = [](Object* o, const char* frag) -> bool {
      return o && o->class_name.find(frag) != std::string::npos;
    };

    // A name-only call (`this.create(...)`) does not push the receiver.
    // Popping the top object there steals an argument.
    std::vector<Value> call_args;
    bool recv_from_path = false;
    if (have_call_recv) {
      call_recv_on_stack = false;
      if (call_recv_value.tag == ValueTag::Obj) recv = call_recv_value.v.o;
      have_call_recv = false;
      recv_from_path = true;
    }
    if (!is_static && call_recv_on_stack && !recv_from_path) {
      if (!stack.empty() && stack.back().tag == ValueTag::Obj) {
        Object* top = stack.back().v.o;
        if (menu_call && class_has(top, "Menu"))
          recv = pop().v.o;
        else if (osd_build && class_has(top, "Osd"))
          recv = pop().v.o;
        else if (!menu_call && !osd_build)
          recv = pop().v.o;
        else if (last_menu)
          recv = last_menu;
        else
          recv = pop().v.o;
      } else if (menu_call && last_menu) {
        recv = last_menu;
      } else if (self) {
        recv = self;
      } else if (!stack.empty()) {
        // A missing field pushes int 0. That value is the receiver slot.
        pop();
      }
      // `saveGame; argc; speedymen[i].botVd; call_on save`.
      if (argc_unset && !stack.empty() && stack.back().tag == ValueTag::Int) {
        const int32_t n = pop().v.i;
        if (n >= 0 && n <= 16) argc = n;
      }
    }
    call_args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      if (stack.empty()) return fail("call arg underflow calling " + owner + "." + name +
                                     " argc=" + std::to_string(argc));
      call_args.insert(call_args.begin(), pop());
    }
    // createText(chas.ratio[i], ...) and the dyno tick labels pass a number
    // into a String parameter. "Ljava.lang.String;" is 18 characters; a
    // 19-wide compare never matched, so those texts stayed blank.
    auto as_string = [&](Value& v) {
      if (v.tag == ValueTag::Float)
        v = Value::make_obj(
            host->tree_new_string(format_script_number(v.v.f)));
      else if (v.tag == ValueTag::Int)
        v = Value::make_obj(host->tree_new_string(std::to_string(v.v.i)));
    };
    if ((name == "createText" || name == "createTextBox") && !call_args.empty())
      as_string(call_args[0]);
    if (!desc.empty() && desc[0] == '(') {
      size_t di = 1;
      for (size_t ai = 0; ai < call_args.size() && di < desc.size() &&
                          desc[di] != ')';
           ++ai) {
        const bool str =
            desc.compare(di, 18, "Ljava.lang.String;") == 0 ||
            desc.compare(di, 18, "Ljava/lang/String;") == 0;
        if (desc[di] == 'L') {
          const auto end = desc.find(';', di);
          di = end == std::string::npos ? desc.size() : end + 1;
        } else if (desc[di] == '[') {
          ++di;
          if (di < desc.size() && desc[di] == 'L') {
            const auto end = desc.find(';', di);
            di = end == std::string::npos ? desc.size() : end + 1;
          } else if (di < desc.size()) {
            ++di;
          }
        } else {
          ++di;
        }
        if (!str) continue;
        as_string(call_args[ai]);
      }
    }
    if (menu_call && last_menu &&
        (!recv || !class_has(recv, "Menu")))
      recv = last_menu;
    if (!is_static && !recv && !recv_from_path) {
      if (menu_call && last_menu)
        recv = last_menu;
      else if (osd_build && self) {
        auto it = self->fields.find("osd");
        if (it != self->fields.end() && it->second.tag == ValueTag::Obj)
          recv = it->second.v.o;
        else
          recv = self;
      } else if (self) {
        recv = self;
      }
    }
    if (osd_build && self && (!recv || !class_has(recv, "Osd"))) {
      auto it = self->fields.find("osd");
      if (it != self->fields.end() && it->second.tag == ValueTag::Obj &&
          it->second.v.o)
        recv = it->second.v.o;
    }
    auto is_osd = [](Object* o) {
      if (!o) return false;
      const std::string& n = o->class_name;
      return n.size() >= 4 && n.compare(n.size() - 4, 4, ".Osd") == 0;
    };
    if (osd_build && !is_osd(recv)) {
      Object* field_osd = nullptr;
      if (self) {
        auto it = self->fields.find("osd");
        if (it != self->fields.end() && it->second.tag == ValueTag::Obj)
          field_osd = it->second.v.o;
      }
      if (field_osd)
        recv = field_osd;
      else if (last_osd)
        recv = last_osd;
    }
    if (is_osd(recv)) last_osd = recv;
    if (recv && owner.empty()) owner = recv->class_name;
    if (owner.empty()) owner = cls.name();
    // invokevirtual: MainMenuDialog.addCustomGroups, not the empty
    // OptionsDialog override. invokespecial (call_super) keeps the super owner.
    // Osd.show / Osd.hide stay on Osd. Rewriting them onto the dialog made
    // backShieldOsd.show() call StringRequesterDialog.show until the depth cap,
    // so the plate, title and OK/Cancel were never finished.
    const bool osd_decl =
        owner.size() >= 4 && owner.compare(owner.size() - 4, 4, ".Osd") == 0;
    // Follow the path receiver (groups → Vector, label → Text). A qualified
    // Vector.size must not be rewritten onto the Osd when the path value was
    // missing. Dialog.show stays on Osd: rewriting it onto the subclass recursed.
    if (recv_from_path && recv && !recv->class_name.empty())
      owner = recv->class_name;
    else if (recv && !is_static && !pending_super && !menu_call && !osd_build &&
             !recv->class_name.empty() && !(osd_decl && is_osd(recv)) &&
             (owner.empty() || owner == cls.name()))
      owner = recv->class_name;
    // super.show() must be the superclass. A NameAndType ref names the
    // class being compiled, so PlayerSetupDialog.show called itself until
    // the invoke depth limit and Object.wait never blocked.
    if (pending_super && !osd_build) {
      const std::string sn = cls.super_name();
      if (!sn.empty()) owner = sn;
    }
    pending_super = false;
    // Prefer Menu/Osd owner from receiver class when method ref owner wrong.
    if (recv && menu_call) owner = "java.render.osd.Menu";
    if (osd_build && recv) owner = "java.render.Osd";

    Value r;
    const bool static_call = is_static && !recv_from_path;
    // A null instance receiver must not become GameLogic.save(String).
    // That call had no filename, so local 0 stayed 0 and the file was "0main",
    // then the racer loop entered save again.
    if ((recv_from_path || call_recv_on_stack) && !recv) {
      call_pushed = false;
      path_static = false;
      saw_class = false;
      path_base_on_stack = false;
      pending_class.clear();
      return {};
    }
    // A call with no receiver and no path is static (save(filename)).
    // A path that evaluated to null is not: that case returned above.
    const bool as_static = static_call || !recv;
    if (!desc.empty()) {
      r = host->tree_call(owner, name, desc, recv, call_args, as_static);
    } else {
      std::vector<Value> full = call_args;
      if (recv && !static_call)
        full.insert(full.begin(), Value::make_obj(recv));
      r = host->tree_call_by_name(owner, name, full, as_static);
    }
    if (name == "createMenu" && r.tag == ValueTag::Obj && r.v.o)
      last_menu = r.v.o;
    call_pushed = r.tag != ValueTag::Void;
    if (call_pushed) push(r);
    path_static = false;
    saw_class = false;
    path_base_on_stack = false;
    pending_class.clear();
    return {};
  };

  auto do_new_init = [&]() -> TreeEvalResult {
    const int argc = pending_argc < 0 ? 0 : pending_argc;
    pending_argc = -1;
    // `new ResourceRef(Osd.RID_BACK)`: type sets pending_new_class, then
    // path+class+field for the arg must not steal the constructed type.
    std::string typ;
    if (!new_types.empty()) {
      typ = new_types.back();
      new_types.pop_back();
    }
    if (typ.empty())
      typ = !pending_new_class.empty() ? pending_new_class : pending_class;
    if (typ.empty()) return fail("new_init without type");
    Object* obj = host->tree_new(typ);
    // Pop ctor args (pushed left→right; stack top = last arg).
    std::vector<Value> stack_args;
    stack_args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      if (stack.empty()) break;
      stack_args.push_back(pop());
    }
    std::reverse(stack_args.begin(), stack_args.end());
    std::vector<Value> ctor_args;
    ctor_args.reserve(stack_args.size() + 1);
    ctor_args.push_back(Value::make_obj(obj));
    for (Value& a : stack_args) ctor_args.push_back(a);
    // Run <init> (host native Style/ResourceRef, or Menu.java TREE).
    host->tree_call_by_name(typ, "<init>", ctor_args, false);
    push(Value::make_obj(obj));
    if (obj && typ.size() >= 4 && typ.compare(typ.size() - 4, 4, ".Osd") == 0)
      last_osd = obj;
    pending_class.clear();
    pending_new_class.clear();
    pending_expr = -1;
    saw_class = false;
    path_static = false;
    return {};
  };

  // OptionsDialog.show builds every menu group in one TREE (main + options +
  // redefine key loop). Comparisons must fold before jumps or `i < N` never
  // exits and this budget is spent on thousands of "?" texts.
  constexpr size_t kMaxSteps = 4000000;
  size_t steps = 0;
  size_t ip = 0;
  // If i++ doesn't stick, `i < N` stays true. Cap backward edges so the
  // rest of the method still runs. TextBox.changeText loops once per word;
  // 80 cuts the catalog description before the last line can take " (...)".
  std::unordered_map<size_t, int> back_edges;
  std::string jump_err;
  auto take_rel = [&](int32_t rel) -> bool {
    if (rel == 0) return true;
    const int64_t next = static_cast<int64_t>(ip) + rel;
    if (next < 0 || next >= static_cast<int64_t>(tree.size())) {
      jump_err = "jump OOB";
      return false;
    }
    if (rel < 0 && !(vm && script_thread_active(*vm)) &&
        ++back_edges[static_cast<size_t>(next)] > 8192) return true;
    ip = static_cast<size_t>(next) - 1;
    return true;
  };
  // expr assign (0x07 / 35) and stmt assign (0x08 / 35). `hideGroup(optionsGroup
  // = endGroup())` is an expression: the field must be stored and the value
  // kept as the call argument.
  auto next_is_assign = [&]() -> bool {
    if (ip + 1 >= tree.size() || !tree[ip + 1].wide || tree[ip + 1].imm != 35)
      return false;
    const uint8_t op = tree[ip + 1].op;
    return op == 0x07 || op == 0x08;
  };
  auto commit_assign = [&](bool keep) {
    put_recv_from_path = false;
    if (pending_arr) {
      Value val = stack.empty() ? Value::make_obj(nullptr) : pop();
      if (pending_arr->is_array && pending_arr_idx >= 0) {
        const size_t need = static_cast<size_t>(pending_arr_idx) + 1;
        if (pending_arr->slots.size() < need) {
          pending_arr->slots.resize(need, Value::make_obj(nullptr));
          pending_arr->fields["length"] =
              Value::make_int(static_cast<int32_t>(pending_arr->slots.size()));
        }
        pending_arr->slots[static_cast<size_t>(pending_arr_idx)] = val;
      }
      pending_arr = nullptr;
      pending_arr_idx = -1;
      if (keep) push(val);
    } else if (pending_local_store >= 0) {
      if (pending_local_store >= static_cast<int>(locals.size()))
        locals.resize(static_cast<size_t>(pending_local_store) + 1);
      Value val = pop();
      locals[static_cast<size_t>(pending_local_store)] = local_value(static_cast<size_t>(pending_local_store), val);
      if (keep) push(val);
      pending_local_store = -1;
    } else if (!pending_field.empty()) {
      Value val = pop();
      Object* recv = self;
      if (have_put_recv) {
        recv = pending_put_recv;
        have_put_recv = false;
        pending_put_recv = nullptr;
      } else if (!stack.empty() && stack.back().tag == ValueTag::Obj)
        recv = pop().v.o;
      bool own_static = false;
      for (const auto& sf : cls.static_fields) {
        if (cls.utf8(sf.name) == pending_field) {
          own_static = true;
          break;
        }
      }
      if (own_static ||
          (saw_class && path_static && !pending_class.empty())) {
        const std::string owner =
            !pending_class.empty() ? pending_class : cls.name();
        host->tree_set_static(owner, pending_field, val);
      } else if (recv) {
        // Integer literals assigned to float fields (`new Vector3(-2, 1, -4)`)
        // stay Int unless coerced here. Natives that only read Float then see 0.
        // An int field keeps a whole number: catalog prices are `(int)`.
        // Gear ratios are a float[] and are not touched here.
        if (pending_field_desc == "F" && val.tag == ValueTag::Int)
          val = Value::make_float(static_cast<float>(val.v.i));
        else if ((pending_field_desc == "I" || pending_field_desc == "B" ||
                  pending_field_desc == "S" || pending_field_desc == "C" ||
                  pending_field_desc == "Z") &&
                 val.tag == ValueTag::Float) {
          int32_t n = static_cast<int32_t>(val.v.f);
          if (pending_field_desc == "Z") n = n != 0 ? 1 : 0;
          val = Value::make_int(n);
        }
        recv->fields[pending_field] = val;
        osd_on_field_write(recv, pending_field);
      }
      saw_class = false;
      if (keep) push(val);
      pending_field.clear();
      pending_field_desc.clear();
      path_static = false;
    }
  };
  for (; ip < tree.size(); ++ip) {
    if(vm && (steps % 2048)==0 && script_thread_checkpoint(*vm))steps=0;
    if (++steps > kMaxSteps) {
      if(vm && script_thread_active(*vm))steps=0;
      else return fail("step budget exceeded");
    }
    const auto& n = tree[ip];
    switch (n.op) {
      case 0x01: {  // local
        if (!n.wide || n.imm >= locals.size())
          return fail("local out of range");
        // `…; local N; stmt assign` stores to local N (buttonStyle / menu m).
        if (next_is_assign()) {
          pending_local_store = static_cast<int>(n.imm);
          break;
        }
        // `nodes++` is local + stmt postinc/postdec. Apply it here; the stmt
        // tag itself has no operand.
        if (ip + 1 < tree.size() && tree[ip + 1].op == 0x08 &&
            tree[ip + 1].wide) {
          const uint32_t tag = tree[ip + 1].imm;
          if (tag >= 0x17 && tag <= 0x1A) {
            const Value cur = locals[n.imm];
            const int32_t v = cur.tag == ValueTag::Int ? cur.v.i : 0;
            const int32_t d = (tag == 0x17 || tag == 0x19) ? 1 : -1;
            locals[n.imm] = Value::make_int(v + d);
            break;
          }
        }
        if (pending_call) {
          push(locals[n.imm]);
          const bool next_method =
              ip + 1 < tree.size() &&
              (tree[ip + 1].op == 0x12 || tree[ip + 1].op == 0x1a);
          const bool next_field =
              ip + 1 < tree.size() &&
              (tree[ip + 1].op == 0x1b || tree[ip + 1].op == 0x1c);
          if (next_method && !stack.empty()) {
            // `label.changeColor` / `group.deactivate`: the local is the receiver.
            call_recv_value = pop();
            have_call_recv = true;
            call_recv_on_stack = false;
          } else if (next_field) {
            // `group.gadget.addElement`: the local is the field base.
            path_base_on_stack = true;
            call_recv_on_stack = false;
          } else {
            call_recv_on_stack = true;
          }
          break;
        }
        push(locals[n.imm]);
        break;
      }
      case 0x02:  // only declarations with an initializer own a trailing store
        if (n.wide) {
          if (local_types.size() <= n.imm) local_types.resize(n.imm + 1);
          if (ip + 1 < tree.size() && tree[ip + 1].op == 0x06) local_types[n.imm] = cls.utf8(tree[ip + 1].imm);
          // Invictus permits uninitialized locals. A declaration inside a
          // loop starts a new lifetime; retaining the preceding iteration's
          // InventoryItem attaches the same can to multiple empty panels.
          if(locals.size()<=n.imm)locals.resize(n.imm+1);
          const auto& type=local_types[n.imm];
          locals[n.imm]=!type.empty()&&(type[0]=='L'||type[0]=='[')
            ?Value::make_obj(nullptr):type=="F"?Value::make_float(0.f):Value::make_int(0);
        }
        if (n.wide && ip + 2 < tree.size() && tree[ip + 2].op == 0x2c)
          pending_init_local = static_cast<int>(n.imm);
        break;
      case 0x03:  // release
        break;
      case 0x04: {  // and_test: true skips the jump so the right side runs
        const Value v = stack.empty() ? Value::make_int(0) : stack.back();
        if (truthy(v) && ip + 1 < tree.size()) ++ip;
        break;
      }
      case 0x05: {  // or_test: false skips the jump so the right side runs
        const Value v = stack.empty() ? Value::make_int(0) : stack.back();
        if (!truthy(v) && ip + 1 < tree.size()) ++ip;
        break;
      }
      case 0x0e: {  // rid — CONS Resource (pack utf8 + local id), not pool index
        if (!n.wide) return fail("rid missing imm");
        try {
          const auto rid = cls.resource(n.imm);
          // Keep the pack. A bare local id is looked up in frontend.rpk first,
          // so MW_Mod:0x0030 was drawn as a frontend charset atlas.
          push(Value::make_int(static_cast<int32_t>(
              rpk_resolve_resource(rid.pack, rid.id))));
        } catch (...) {
          push(Value::make_int(0));
        }
        break;
      }
      case 0x06: {  // type (utf8 descriptor)
        if (!n.wide) return fail("type missing imm");
        const std::string raw = cls.utf8(n.imm);
        // `new T[n]`: size, expr 31, then the `[...]` descriptor.
        if (pending_expr == 31 && !raw.empty() && raw[0] == '[') {
          int32_t len = 0;
          if (!stack.empty()) {
            const Value top = pop();
            if (top.tag == ValueTag::Int) len = top.v.i;
            else if (top.tag == ValueTag::Float)
              len = static_cast<int32_t>(top.v.f);
          }
          if (len < 0) len = 0;
          if (len > (1 << 20)) len = 1 << 20;
          Object* arr = host->tree_new(raw);
          arr->is_array = true;
          const bool refs = raw.size() >= 2 && (raw[1] == 'L' || raw[1] == '[');
          arr->slots.assign(static_cast<size_t>(len),
                            refs ? Value::make_obj(nullptr)
                                 : Value::make_int(0));
          arr->fields["length"] = Value::make_int(len);
          push(Value::make_obj(arr));
          pending_expr = -1;
          pending_class = raw;
          break;
        }
        pending_class = strip_type_desc(raw);
        // Remember constructed type across getstatic class ops in ctor args.
        if (pending_expr == 30) {
          pending_new_class = pending_class;
          if (!new_types.empty()) new_types.back() = pending_class;
        }
        // `prev instanceof RaceSetup` — local, expr 32, then this type.
        if (pending_expr == 32) {
          Object* obj = nullptr;
          if (!stack.empty() && stack.back().tag == ValueTag::Obj)
            obj = pop().v.o;
          const bool ok = host->tree_instance_of(obj, pending_class);
          push(Value::make_int(ok ? 1 : 0));
          pending_expr = -1;
          break;
        }
        // `(int)(price * ratio)`. A declaration's type node follows opcode
        // 0x02 and does not convert whatever happens to sit on the stack.
        if ((ip == 0 || tree[ip - 1].op != 0x02) && !stack.empty()) {
          Value& top = stack.back();
          if ((raw == "I" || raw == "B" || raw == "S" || raw == "C" ||
               raw == "Z") &&
              top.tag == ValueTag::Float) {
            int32_t n = static_cast<int32_t>(top.v.f);
            if (raw == "Z") n = n != 0 ? 1 : 0;
            top = Value::make_int(n);
          } else if (raw == "F" && top.tag == ValueTag::Int) {
            top = Value::make_float(static_cast<float>(top.v.i));
          }
        }
        break;
      }
      case 0x07: {  // expr tag
        pending_expr = n.wide ? static_cast<int>(n.imm) : -1;
        // ++ / -- on the variable just read. 23/24 leave the old value
        // (`x++`); 25/26 push the new one (`++x`, as in `++lastInLine`).
        if (pending_expr >= 23 && pending_expr <= 26 && ip > 0 &&
            !stack.empty()) {
          const int32_t step =
              (pending_expr == 23 || pending_expr == 25) ? 1 : -1;
          const bool prefix = pending_expr >= 25;
          const Value cur = stack.back();
          Value next = cur;
          if (cur.tag == ValueTag::Float)
            next.v.f = cur.v.f + static_cast<float>(step);
          else
            next = Value::make_int(
                (cur.tag == ValueTag::Int ? cur.v.i : 0) + step);
          const auto& prev = tree[ip - 1];
          if (prev.op == 0x01 && prev.wide && prev.imm < locals.size()) {
            locals[prev.imm] = next;
          } else if ((prev.op == 0x1b || prev.op == 0x1c) &&
                     last_ifield_fresh && last_ifield_recv &&
                     !last_ifield_name.empty()) {
            last_ifield_recv->fields[last_ifield_name] = next;
            last_ifield_fresh = false;
          } else if (prev.op == 0x20 && last_arr && last_arr->is_array &&
                     last_arr_idx >= 0 &&
                     static_cast<size_t>(last_arr_idx) < last_arr->slots.size()) {
            last_arr->slots[static_cast<size_t>(last_arr_idx)] = next;
          }
          if (prefix) stack.back() = next;
          pending_expr = -1;
          break;
        }
        if (pending_expr == 35) {
          commit_assign(true);
          pending_expr = -1;
          break;
        }
        // Splash createHotkey: VIRTUAL|HK_STATIC with no following value op.
        if (pending_expr == 3 && stack.size() >= 2) do_binop(3);
        // Unary ops apply immediately (no trailing `value`): bnot/lnot/neg.
        if (pending_expr >= 20 && pending_expr <= 22 && !stack.empty()) {
          Value a = pop();
          if (pending_expr == 22) {  // neg
            if (a.tag == ValueTag::Float)
              push(Value::make_float(-a.v.f));
            else
              push(Value::make_int(-(a.tag == ValueTag::Int ? a.v.i : 0)));
          } else if (pending_expr == 21) {  // lnot
            push(Value::make_int(truthy(a) ? 0 : 1));
          } else {  // bnot
            const int32_t ai = a.tag == ValueTag::Int ? a.v.i : 0;
            push(Value::make_int(~ai));
          }
          pending_expr = -1;
          break;
        }
        // expr call — Garage `g = m.addItem(...)` uses this (not stmt call).
        if (pending_expr == 27) {
          TreeEvalResult er = finish_call();
          if (!er.error.empty()) return er;
          pending_expr = -1;
          break;
        }
        if (pending_expr == 28) pending_expr = -1;  // literal marker
        // The tag follows both operands (`0 == d.display()`, `a && b`).
        // Folding only at jump_false left `reallyExit = (0 == display())`
        // storing the raw 0.
        if (pending_expr == 1 || pending_expr == 2) {
          if (stack.size() >= 2) {
            const Value b = pop();
            const Value a = pop();
            const bool r = pending_expr == 1 ? (truthy(a) || truthy(b))
                                             : (truthy(a) && truthy(b));
            push(Value::make_int(r ? 1 : 0));
          }
          pending_expr = -1;
        } else if ((pending_expr >= 3 && pending_expr <= 14) ||
                   pending_expr == 15 || pending_expr == 16 ||
                   pending_expr == 17 || pending_expr == 18 ||
                   pending_expr == 19) {
          if (stack.size() >= 2) do_binop(pending_expr);
        }
        if (pending_expr == 30) {
          // `new` — type carrier follows. Push a slot so a nested new can
          // finish without erasing this one.
          new_types.emplace_back();
        }
        break;
      }
      case 0x08: {  // stmt tag
        const int tag = n.wide ? static_cast<int>(n.imm) : -1;
        if (tag == 27) {  // call — statement form drops the return
          TreeEvalResult er = finish_call();
          if (!er.error.empty()) return er;
          if (call_pushed) (void)pop();
        } else if (tag == 35) {  // assign — local store or putfield
          // Keep value when it is an argument of a following call
          // (hideGroup((menuGroup2 = endGroup()))). A later statement call
          // with argc 0, such as setFonts() after `render = new GfxEngine()`,
          // must not reuse that object as its receiver.
          bool as_expr = false;
          int follow_argc = -1;
          bool saw_call = false;
          for (size_t j = ip + 1; j < tree.size() && j < ip + 12; ++j) {
            // `osd = new Osd(); osd.createBG(pic)` — the next field load is
            // a new expression. Only `(x = endGroup())` used as a call
            // argument has the call opcode before any other load.
            if (!saw_call &&
                (tree[j].op == 0x01 || tree[j].op == 0x1b ||
                 tree[j].op == 0x1c || tree[j].op == 0x07 ||
                 tree[j].op == 0x0e))
              break;
            if (tree[j].op == 0x24 || tree[j].op == 0x21 ||
                tree[j].op == 0x22 || tree[j].op == 0x23 ||
                tree[j].op == 0x25 || tree[j].op == 0x26)
              saw_call = true;
            if (tree[j].op == 0x27 && tree[j].wide && j + 1 < tree.size() &&
                (tree[j + 1].op == 0x24 || tree[j + 1].op == 0x21 ||
                 tree[j + 1].op == 0x22 || tree[j + 1].op == 0x23 ||
                 tree[j + 1].op == 0x25 || tree[j + 1].op == 0x26))
              follow_argc = static_cast<int>(tree[j].imm);
            if (tree[j].op == 0x08 && tree[j].wide && tree[j].imm == 27) {
              as_expr = follow_argc > 0;
              break;
            }
            if (tree[j].op == 0x08 && tree[j].wide && tree[j].imm == 35)
              break;  // another assign — statement form
          }
          // putstatic only after path+class. Sticky path_static was writing
          // mainGroup/optionsGroup onto the class and leaving the instance
          // field empty, so changeMode always got 0.
          commit_assign(as_expr);
        } else if (tag >= 23 && tag <= 26 && ip > 0 &&
                   (tree[ip - 1].op == 0x1b || tree[ip - 1].op == 0x1c)) {
          // Statement `actId++` / `++actId`. The field load left the old
          // value on the stack; write the stepped value back. Local ++/-- is
          // already applied by the local opcode.
          const int32_t step = (tag == 23 || tag == 25) ? 1 : -1;
          const Value cur = stack.empty() ? Value::make_int(0) : pop();
          Value next = cur;
          if (cur.tag == ValueTag::Float)
            next.v.f = cur.v.f + static_cast<float>(step);
          else
            next = Value::make_int(
                (cur.tag == ValueTag::Int ? cur.v.i : 0) + step);
          if (last_ifield_fresh && last_ifield_recv &&
              !last_ifield_name.empty()) {
            last_ifield_recv->fields[last_ifield_name] = next;
            last_ifield_fresh = false;
          }
        } else if (tag == 34) {  // ctor stmt — often after super_init
          // no-op: ctor body continues
        } else if ((tag >= 36 && tag <= 40) || (tag >= 44 && tag <= 46)) {
          // Compound assign. The target was just pushed (local or field);
          // the rhs sits under it.
          if (ip > 0 && stack.size() >= 2) {
            const auto& prev = tree[ip - 1];
            const Value lhs = pop();
            const Value rhs = pop();
            const Value result = combine_assign(tag, lhs, rhs);
            if (prev.op == 0x01 && prev.wide) {
              if (prev.imm >= locals.size())
                locals.resize(static_cast<size_t>(prev.imm) + 1);
              locals[prev.imm] = local_value(prev.imm, result);
            } else if ((prev.op == 0x1b || prev.op == 0x1c) && prev.wide) {
              // `int fee += length * 0.03` is computed in float, then Java
              // narrows it back to the field. Leaving the float in towCarFee
              // printed $32.377 and stored that float over player.money.
              Value stored = result;
              try {
                const auto ref = cls.reference(prev.imm);
                const std::string& desc = ref.descriptor;
                if ((desc == "I" || desc == "B" || desc == "S" || desc == "C" ||
                     desc == "Z") &&
                    stored.tag == ValueTag::Float) {
                  int32_t n = static_cast<int32_t>(stored.v.f);
                  if (desc == "Z") n = n != 0 ? 1 : 0;
                  stored = Value::make_int(n);
                } else if (desc == "F" && stored.tag == ValueTag::Int) {
                  stored = Value::make_float(static_cast<float>(stored.v.i));
                }
                if (last_ifield_fresh && last_ifield_recv) {
                  last_ifield_recv->fields[last_ifield_name] = stored;
                  last_ifield_fresh = false;
                } else {
                  const std::string owner =
                      ref.owner.empty() ? cls.name() : ref.owner;
                  host->tree_set_static(owner, ref.name, stored);
                }
              } catch (...) {
                if (last_ifield_fresh && last_ifield_recv) {
                  last_ifield_recv->fields[last_ifield_name] = result;
                  last_ifield_fresh = false;
                }
              }
            } else if (last_arr && last_arr->is_array && last_arr_idx >= 0 &&
                       (prev.op == 0x20 || prev.op == 0x07)) {
              // `a[i] -= x` may sit an expr-tag between the load and the
              // statement. Write the result back into the slot just read.
              const size_t need = static_cast<size_t>(last_arr_idx) + 1;
              if (last_arr->slots.size() < need)
                last_arr->slots.resize(need, Value::make_obj(nullptr));
              last_arr->slots[static_cast<size_t>(last_arr_idx)] = result;
              last_arr = nullptr;
              last_arr_idx = -1;
            }
          }
        }
        pending_expr = -1;
        break;
      }
      case 0x09: {  // string
        if (!n.wide) return fail("str missing imm");
        push(Value::make_obj(host->tree_new_string(cls.utf8(n.imm))));
        break;
      }
      case 0x0a: {
        if (!n.wide) return fail("float missing imm");
        push(Value::make_float(bits_to_float(n.imm)));
        break;
      }
      case 0x0b: {
        if (!n.wide) return fail("int missing imm");
        push(Value::make_int(static_cast<int32_t>(n.imm)));
        break;
      }
      case 0x0c:
        push(Value::make_obj(nullptr));
        break;
      case 0x0d:  // boolean literal. removeElement returns true/false as int.
        if (!n.wide) return fail("bool missing imm");
        push(Value::make_int(n.imm ? 1 : 0));
        break;
      case 0x11: {  // path
        // After call/call_on this is the name-path header; the segments run
        // as the call. Otherwise imm is the segment count of a field path
        // (`player.controller`, `GameLogic.gameMode`) and only the last
        // field is pushed.
        path_static = true;
        if (pending_call || !n.wide || n.imm == 0) break;
        // `optionsGroup = endGroup()` — the path is the store target.
        {
          const size_t after = ip + 1 + static_cast<size_t>(n.imm);
          if (after < tree.size() && tree[after].wide && tree[after].imm == 35 &&
              (tree[after].op == 0x07 || tree[after].op == 0x08)) {
            // Store paths still evaluate intermediate instance receivers.
            // A later class carrier explicitly selects a static path.
            path_static = false;
            put_recv_from_path = true;
            break;
          }
        }
        const size_t path_ip = ip;
        const int segs = static_cast<int>(n.imm);
        Object* cur = self;
        bool on_class = false;
        std::string owner;
        Value result = Value::make_void();
        bool any = false;
        bool abort = false;
        for (int s = 0; s < segs && ip + 1 < tree.size(); ++s) {
          ++ip;
          const auto& seg = tree[ip];
          if (seg.op == 0x19 && seg.wide) {
            owner = cls.class_name(seg.imm);
            on_class = true;
            cur = nullptr;
            continue;
          }
          std::string fname;
          std::string fdesc;
          std::string field_owner;
          if ((seg.op == 0x1b || seg.op == 0x1c) && seg.wide) {
            try {
              const auto ref = cls.reference(seg.imm);
              fname = ref.name;
              fdesc = ref.descriptor;
              field_owner = ref.owner;
              if (owner.empty() && !ref.owner.empty()) owner = ref.owner;
            } catch (...) {
              abort = true;
              break;
            }
          } else if (seg.op == 0x12 && seg.wide) {
            fname = cls.utf8(seg.imm);
          } else if (seg.op == 0x01 && seg.wide) {
            // `sty.charset` — the local is the object, the next segment is
            // the field. Pushing the local as well drops the label argument.
            if (seg.imm >= locals.size()) {
              abort = true;
              break;
            }
            result = locals[seg.imm];
            cur = result.tag == ValueTag::Obj ? result.v.o : nullptr;
            on_class = false;
            any = true;
            continue;
          } else {
            abort = true;
            break;
          }
          // `player.name` in a static method has no class opcode. The first
          // segment is GameLogic.player (static); reading it on `this`
          // (null) made the career folder "null-1" and wrote an empty file.
          const std::string kind_owner =
              !field_owner.empty() ? field_owner : owner;
          const int kind =
              fname.empty() ? 0 : host->tree_field_kind(kind_owner, fname);
          if (on_class || kind == 1) {
            const std::string so = !kind_owner.empty()
                                       ? kind_owner
                                       : (owner.empty() ? cls.name() : owner);
            result = host->tree_get_static(so, fname);
            last_ifield_fresh = false;
            last_ifield_recv = nullptr;
            last_ifield_name.clear();
            on_class = false;
            owner.clear();
          } else {
            const size_t mark = stack.size();
            push_instance_field(cur, fname, fdesc);
            result = stack.back();
            stack.resize(mark);
          }
          any = true;
          cur = result.tag == ValueTag::Obj ? result.v.o : nullptr;
        }
        if (abort) {
          ip = path_ip;
          break;
        }
        if (any) push(result);
        path_static = false;
        saw_class = false;
        pending_class.clear();
        break;
      }
      case 0x12: {  // name (utf8 method name)
        if (!n.wide) return fail("name missing imm");
        pending_method_name = cls.utf8(n.imm);
        // `name` is an unqualified call — drop any leftover method-ref descriptor
        // (e.g. prior endGroup ()I must not stick onto later addItem/hideGroup).
        pending_method_desc.clear();
        // Only bind owner from path+class (static). Instance name uses recv.
        if (saw_class && !pending_class.empty())
          pending_method_owner = pending_class;
        break;
      }
      case 0x14: {  // jump relative
        if (!n.wide) break;
        if (!take_rel(static_cast<int32_t>(n.imm))) return fail(jump_err);
        break;
      }
      case 0x15: {  // jump_false
        // for (i < N): expr lt sits on the stack with no trailing `value`.
        if (pending_expr == 7 && stack.size() >= 2) do_eq();
        else if (pending_expr >= 6 && pending_expr <= 11 && stack.size() >= 2)
          do_binop(pending_expr);
        Value cond = stack.empty() ? Value::make_int(0) : pop();
        if (!truthy(cond) && n.wide) {
          if (!take_rel(static_cast<int32_t>(n.imm))) return fail(jump_err);
        }
        pending_expr = -1;
        break;
      }
      case 0x16: {  // jump_true
        if (pending_expr == 7 && stack.size() >= 2) do_eq();
        else if (pending_expr >= 6 && pending_expr <= 11 && stack.size() >= 2)
          do_binop(pending_expr);
        Value cond = stack.empty() ? Value::make_int(0) : pop();
        if (truthy(cond) && n.wide) {
          if (!take_rel(static_cast<int32_t>(n.imm))) return fail(jump_err);
        }
        pending_expr = -1;
        break;
      }
      case 0x17: {  // switch case: compare label, retaining the subject on a miss
        if(stack.size()<2)return fail("case stack underflow");
        Value label=pop();
        if(values_equal(label,stack.back())){pop();if(n.wide&&!take_rel(static_cast<int32_t>(n.imm)))return fail(jump_err);}
        pending_expr=-1;break;
      }
      case 0x18:  // pop
        (void)pop();
        break;
      case 0x19: {  // class
        if (!n.wide) return fail("class missing imm");
        if(pending_expr==32) {
          Object* object=!stack.empty()&&stack.back().tag==ValueTag::Obj?pop().v.o:nullptr;
          push(Value::make_int(host->tree_instance_of(object,cls.class_name(n.imm))?1:0));
          pending_expr=-1;pending_class.clear();path_static=false;saw_class=false;
          break;
        }
        if (pending_expr == 33) {
          // A reference cast keeps the object on the stack. Its class
          // carrier is not a static-access prefix: ((Slider)e.gadget).value
          // must read the slider, and the following assignment stays on this.
          pending_expr = -1;
          pending_class.clear();
          path_static = false;
          saw_class = false;
          break;
        }
        pending_class = cls.class_name(n.imm);
        path_static = true;
        saw_class = true;
        break;
      }
      case 0x1a: {  // method ref
        if (!n.wide) return fail("method missing imm");
        try {
          const auto ref = cls.reference(n.imm);
          pending_method_owner = ref.owner;
          pending_method_name = ref.name;
          pending_method_desc = ref.descriptor;
        } catch (const std::exception&) {
          // Soft sometimes encodes a bare Utf8 method name here.
          try {
            pending_method_owner = cls.name();
            pending_method_name = cls.utf8(n.imm);
            pending_method_desc.clear();
          } catch (const std::exception& e) {
            return fail(std::string("method ref: ") + e.what());
          }
        }
        break;
      }
      case 0x1b: {  // field ref
        if (!n.wide) return fail("field missing imm");
        TufaClass::Ref ref;
        try {
          ref = cls.reference(n.imm);
        } catch (const std::exception&) {
          try {
            ref = TufaClass::Ref{cls.name(), cls.utf8(n.imm), {}};
          } catch (const std::exception& e) {
            return fail(std::string("field ref: ") + e.what());
          }
        }
        pending_field = ref.name;
        pending_field_desc = ref.descriptor;
        if (!ref.owner.empty()) pending_class = ref.owner;

        // Call path `groups.addElement` / `group.gadget.addElement`. A foreign
        // owner is still an instance field. Reading it as getstatic pushed
        // void, finish_call fell back to this Osd, and hideGroup never ran.
        if (pending_call && !next_is_assign()) {
          // `actualState.exit(state)` — actualState is a static. An instance
          // read is null, so exit/enter were looked up on GameLogic. The
          // splash plate stayed up and MainMenu.enter never built the menu.
          bool own_static = false;
          if (ref.owner.empty() || ref.owner == cls.name()) {
            for (const auto& sf : cls.static_fields) {
              if (cls.utf8(sf.name) == ref.name) {
                own_static = true;
                break;
              }
            }
          }
          if (own_static || saw_class) {
            const std::string so =
                ref.owner.empty()
                    ? (pending_class.empty() ? cls.name() : pending_class)
                    : ref.owner;
            call_recv_value = host->tree_get_static(so, ref.name);
            have_call_recv = true;
            call_recv_on_stack = false;
            path_base_on_stack = false;
            pending_field.clear();
            pending_field_desc.clear();
            if (pending_class == cls.name()) pending_class.clear();
            saw_class = false;
            path_static = false;
            break;
          }
          Object* base = self;
          const bool foreign =
              !ref.owner.empty() && ref.owner != cls.name();
          if (have_call_recv) {
            // `label.txt.changeText(arg)`: this segment continues the path.
            // The stack top is the argument, not the receiver.
            base = call_recv_value.tag == ValueTag::Obj ? call_recv_value.v.o
                                                       : nullptr;
          } else if ((foreign || path_base_on_stack) && !stack.empty() &&
                     stack.back().tag == ValueTag::Obj) {
            base = pop().v.o;
          }
          path_base_on_stack = false;
          push_instance_field(base, pending_field, pending_field_desc);
          if (!stack.empty()) {
            call_recv_value = pop();
            have_call_recv = true;
          }
          call_recv_on_stack = false;
          pending_field.clear();
          pending_field_desc.clear();
          if (pending_class == cls.name()) pending_class.clear();
          saw_class = false;
          path_static = false;
          break;
        }

        const bool is_assign = next_is_assign();

        if (is_assign) {
          // `obj.field = value` pushes the value, then the receiver. The
          // receiver's path, not the field owner's package, distinguishes it
          // from an object value in `this.field = value`.
          const bool foreign =
              !ref.owner.empty() && ref.owner != cls.name();
          if ((put_recv_from_path || foreign) && !saw_class && !stack.empty() &&
              stack.back().tag == ValueTag::Obj) {
            pending_put_recv = pop().v.o;
            have_put_recv = true;
          }
          put_recv_from_path = false;
        } else if (!is_assign) {
          const bool foreign =
              !ref.owner.empty() && ref.owner != cls.name();
          // Same-class statics omit path+class (Garage.CMD_HITTHESTREET).
          bool own_static = false;
          if (!foreign) {
            for (const auto& sf : cls.static_fields) {
              if (cls.utf8(sf.name) == pending_field) {
                own_static = true;
                break;
              }
            }
          }
          // Foreign instance reads (`player.money`, `player.name`,
          // `speedymen.length`) share this opcode with foreign statics
          // (`GameLogic.player`). A blanket getstatic stored zeros in the
          // career file and built the folder name "null-1".
          bool as_static = saw_class || own_static;
          if (foreign && !saw_class) {
            const int kind = host->tree_field_kind(ref.owner, pending_field);
            as_static = kind == 1;
          }
          if (as_static) {
            const std::string owner =
                ref.owner.empty()
                    ? (pending_class.empty() ? cls.name() : pending_class)
                    : ref.owner;
            push(host->tree_get_static(owner, pending_field));
            last_ifield_fresh = false;
            pending_field.clear();
            pending_field_desc.clear();
            saw_class = false;
            path_static = false;
          } else if (ip + 1 < tree.size() && tree[ip + 1].op == 0x28) {
            // Integer.intValue: field + value
          } else {
            // Immediate instance getfield (Splash pic/osd, MainMenu mmd).
            // Same-class fields read `this`. A foreign owner (`arr.length`)
            // takes the object already on the stack.
            Object* recv = self;
            const bool foreign_inst =
                !ref.owner.empty() && ref.owner != cls.name();
            if (foreign_inst && !stack.empty() &&
                stack.back().tag == ValueTag::Obj && !path_static)
              recv = pop().v.o;
            push_instance_field(recv, pending_field, pending_field_desc);
            if (recv && !pending_field.empty()) {
              last_ifield_recv = recv;
              last_ifield_name = pending_field;
              last_ifield_fresh = true;
            }
            pending_field.clear();
            pending_field_desc.clear();
            path_static = false;
            // Keep pending_class only for static; instance field is not a call owner.
            if (pending_class == cls.name()) pending_class.clear();
          }
          if (pending_call) call_recv_on_stack = true;
        }
        break;
      }
      case 0x1c: {  // field quick — get, or put when the next stmt is assign
        if (!n.wide) return fail("getfield missing imm");
        const auto ref = cls.reference(n.imm);
        // Own statics (CMD_EXIT and the other menu commands) are read with
        // this opcode and no class prefix. An instance lookup yields 0, so
        // `command == CMD_EXIT` never matches.
        bool own_static = false;
        for (const auto& sf : cls.static_fields) {
          if (cls.utf8(sf.name) == ref.name) {
            own_static = true;
            break;
          }
        }
        if (next_is_assign()) {
          pending_field = ref.name;
          pending_field_desc = ref.descriptor;
          if (!stack.empty() && stack.back().tag == ValueTag::Obj) {
            pending_put_recv = pop().v.o;
            have_put_recv = true;
          }
          break;
        }
        // `element.MapName` and `CMD_EXIT` share this opcode. TrackSelect
        // declares its own static MapName, so a name match is not enough:
        // a receiver on the stack that actually has the field wins.
        if (!stack.empty() && stack.back().tag == ValueTag::Obj &&
            stack.back().v.o) {
          Object* recv = stack.back().v.o;
          auto iit = recv->fields.find(ref.name);
          const bool on_inst = iit != recv->fields.end();
          Value on_class = Value::make_void();
          if (!on_inst)
            on_class = host->tree_get_static(recv->class_name, ref.name);
          if (on_inst || on_class.tag != ValueTag::Void) {
            pop();
            if (on_inst)
              push(iit->second);
            else
              push(on_class);
            last_ifield_recv = recv;
            last_ifield_name = ref.name;
            last_ifield_fresh = on_inst;
            break;
          }
        }
        if (own_static) {
          const std::string owner =
              ref.owner.empty() ? cls.name() : ref.owner;
          push(host->tree_get_static(owner, ref.name));
          last_ifield_fresh = false;
          break;
        }
        Object* recv = self;
        if (!stack.empty() && stack.back().tag == ValueTag::Obj)
          recv = pop().v.o;
        if (!recv) {
          push(Value::make_int(0));
          break;
        }
        last_ifield_recv = recv;
        last_ifield_name = ref.name;
        last_ifield_fresh = true;
        auto it = recv->fields.find(ref.name);
        if (it == recv->fields.end())
          push(Value::make_int(0));
        else
          push(it->second);
        break;
      }
      case 0x20: {  // array[index]; store when the next node is assign
        Value top = pop();
        Value under = pop();
        // Either order: the array is the object, the index is the number.
        // CLUBNAMES[0] stayed null when the index was taken as the array.
        Value arr_v = top;
        Value idx_v = under;
        const bool top_arr = top.tag == ValueTag::Obj && top.v.o && top.v.o->is_array;
        const bool under_arr =
            under.tag == ValueTag::Obj && under.v.o && under.v.o->is_array;
        if (!top_arr && under_arr) {
          arr_v = under;
          idx_v = top;
        }
        Object* arr = arr_v.tag == ValueTag::Obj ? arr_v.v.o : nullptr;
        const int32_t idx = idx_v.tag == ValueTag::Int
                                ? idx_v.v.i
                                : (idx_v.tag == ValueTag::Float
                                       ? static_cast<int32_t>(idx_v.v.f)
                                       : 0);
        if (next_is_assign()) {
          pending_arr = arr;
          pending_arr_idx = idx;
          break;
        }
        Value el = Value::make_obj(nullptr);
        if (arr && arr->is_array && idx >= 0 &&
            static_cast<size_t>(idx) < arr->slots.size())
          el = arr->slots[static_cast<size_t>(idx)];
        last_arr = arr;
        last_arr_idx = idx;
        push(el);
        break;
      }
      case 0x21: {  // new_init
        TreeEvalResult er = do_new_init();
        if (!er.error.empty()) return er;
        break;
      }
      case 0x22:  // this(args) — Dialog(ctrl, flags, title, buttons) chains here
      case 0x23: {  // super(args)
        const int argc = pending_argc < 0 ? 0 : pending_argc;
        pending_argc = -1;
        std::vector<Value> stack_args;
        stack_args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
          if (stack.empty()) break;
          stack_args.push_back(pop());
        }
        std::reverse(stack_args.begin(), stack_args.end());
        std::string typ = cls.name();
        if (n.op == 0x23) {
          typ = cls.super_name();
          if (typ.empty()) break;
        }
        std::vector<Value> ctor_args;
        ctor_args.reserve(stack_args.size() + 1);
        ctor_args.push_back(Value::make_obj(self));
        for (Value& a : stack_args) ctor_args.push_back(std::move(a));
        host->tree_call_by_name(typ, "<init>", ctor_args, false);
        break;
      }
      case 0x24:  // call — argc already via push_int
        pending_call = true;
        pending_super = false;
        call_recv_on_stack = false;
        have_call_recv = false;
        path_base_on_stack = false;
        break;
      case 0x25:  // call_on — receiver is the computed value on the stack
      case 0x26:  // call_super
        pending_call = true;
        pending_super = n.op == 0x26;
        call_recv_on_stack = n.op == 0x25;
        have_call_recv = false;
        path_base_on_stack = false;
        if (n.wide) {
          pending_method_name = cls.utf8(n.imm);
          if (n.op == 0x26) {
            const std::string sn = cls.super_name();
            if (!sn.empty()) pending_method_owner = sn;
          }
        }
        break;
      case 0x2c:  // init marker
        break;
      case 0x27: {  // push_int (often argc)
        if (!n.wide) return fail("push_int missing imm");
        const int32_t v = static_cast<int32_t>(n.imm);
        // Heuristic: small non-negative before call/new_init is argc.
        if (ip + 1 < tree.size() &&
            (tree[ip + 1].op == 0x24 || tree[ip + 1].op == 0x21 ||
             tree[ip + 1].op == 0x22 || tree[ip + 1].op == 0x23 ||
             tree[ip + 1].op == 0x25 || tree[ip + 1].op == 0x26)) {
          pending_argc = v;
        } else {
          push(Value::make_int(v));
        }
        break;
      }
      case 0x28: {  // value
        if (pending_expr == 7) {
          do_eq();
          break;
        }
        if (!pending_field.empty()) {
          Object* recv = self;
          if (!locals.empty() && locals[0].tag == ValueTag::Obj)
            recv = locals[0].v.o;
          if (!stack.empty() && stack.back().tag == ValueTag::Obj)
            recv = pop().v.o;
          last_ifield_recv = recv;
          last_ifield_name = pending_field;
          last_ifield_fresh = recv != nullptr;
          if (!recv) {
            push(Value::make_int(0));
          } else {
            auto it = recv->fields.find(pending_field);
            if (it == recv->fields.end()) {
              if (pending_field_desc == "F")
                push(Value::make_float(0.f));
              else if (!pending_field_desc.empty() &&
                       pending_field_desc[0] == 'L')
                push(Value::make_obj(nullptr));
              else
                push(Value::make_int(0));
            } else {
              push(it->second);
            }
          }
          pending_field.clear();
          pending_field_desc.clear();
          break;
        }
        // Binary ops subset
        if (pending_expr >= 3 && pending_expr <= 19 && stack.size() >= 2) {
          Value b = pop();
          Value a = pop();
          const bool cmp_f =
              a.tag == ValueTag::Float || b.tag == ValueTag::Float;
          if (cmp_f && pending_expr >= 8 && pending_expr <= 11) {
            auto num = [](const Value& v) -> float {
              if (v.tag == ValueTag::Float) return v.v.f;
              if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
              return 0.f;
            };
            const float af = num(a);
            const float bf = num(b);
            const bool r = pending_expr == 8    ? af >= bf
                           : pending_expr == 9  ? af <= bf
                           : pending_expr == 10 ? af > bf
                                                : af < bf;
            push(Value::make_int(r ? 1 : 0));
            pending_expr = -1;
            break;
          }
          int32_t ai = a.tag == ValueTag::Int ? a.v.i : 0;
          int32_t bi = b.tag == ValueTag::Int ? b.v.i : 0;
          int32_t r = 0;
          switch (pending_expr) {
            case 3:
              r = ai | bi;
              break;  // bor
            case 4:
              r = ai ^ bi;
              break;
            case 12:
              r = static_cast<int32_t>(static_cast<uint32_t>(ai) << (static_cast<uint32_t>(bi) & 31u));
              break;
            case 13:
              r = ai >> (static_cast<uint32_t>(bi) & 31u);
              break;
            case 14:
              r = static_cast<int32_t>(static_cast<uint32_t>(ai) >> (static_cast<uint32_t>(bi) & 31u));
              break;
            case 5:
              r = ai & bi;
              break;  // band
            // A float operand compares by value, as in do_binop. Reading
            // floats as integer 0 made every float `!=` false and every
            // float `==` true on this path.
            case 6:
              if (a.tag == ValueTag::Obj || b.tag == ValueTag::Obj || cmp_f)
                r = values_equal(a, b) ? 0 : 1;
              else
                r = ai != bi;
              break;
            case 7:
              r = cmp_f ? (values_equal(a, b) ? 1 : 0) : ai == bi;
              break;
            case 8:
              r = ai >= bi;
              break;
            case 9:
              r = ai <= bi;
              break;
            case 10:
              r = ai > bi;
              break;
            case 11:
              r = ai < bi;
              break;
            case 15:
              r = ai - bi;
              break;
            case 16:
              r = ai + bi;
              break;
            case 18:
              r = bi ? ai / bi : 0;
              break;
            case 19:
              r = ai * bi;
              break;
            default:
              return fail("unsupported expr tag " + std::to_string(pending_expr));
          }
          push(Value::make_int(r));
          pending_expr = -1;
          break;
        }
        if (pending_expr == 30) {  // new — type follows; wait for new_init
          break;
        }
        // `return nodes` is local + value: the local is already on the stack.
        if (pending_field.empty() && pending_expr < 0) break;
        return fail("value without eq/binop");
      }
      case 0x29:  // push_void
        push(Value::make_void());
        break;
      case 0x10:  // end — field initializer expression
        out.ok = true;
        out.value = stack.empty() ? Value::make_void() : pop();
        return out;
      case 0x2a:  // ret void
        out.ok = true;
        out.value = Value::make_void();
        return out;
      case 0x2b: {  // ret_value
        if (stack.empty()) {
          out.ok = true;
          out.value = Value::make_void();
          return out;
        }
        out.ok = true;
        out.value = pop();
        return out;
      }
      case 0x2d: {  // store local
        // `int x = expr` closes with this node. The slot is the declare
        // that the init marker belongs to, not this node's imm.
        uint32_t slot = 0;
        if (pending_init_local >= 0) {
          slot = static_cast<uint32_t>(pending_init_local);
          pending_init_local = -1;
        } else if (n.wide) {
          slot = n.imm;
        }
        if (slot >= locals.size()) locals.resize(slot + 1);
        locals[slot] = local_value(slot, pop());
        break;
      }
      default: {
        char code[16];std::snprintf(code,sizeof(code),"%02x",static_cast<unsigned>(n.op));
        return fail(std::string("unsupported op 0x")+code+" line="+std::to_string(n.line));
      }
    }
  }
  return fail("missing return");
}

}  // namespace slrr
