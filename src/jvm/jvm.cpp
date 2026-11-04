#include "slrr/jvm.hpp"
#include "slrr/rpk_index.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace slrr {
namespace {
Value declared_return(Value value, const std::string& descriptor) {
  // TREE expressions retain their numeric type. The method boundary applies
  // the declared conversion (e.g. Catalog InventoryItem_Part.getPrice(): int).
  const auto end=descriptor.find(')');
  if(value.tag!=ValueTag::Float || end==std::string::npos || end+1>=descriptor.size())return value;
  const char type=descriptor[end+1];
  if(type!='I' && type!='B' && type!='S' && type!='C' && type!='Z')return value;
  const double number=value.v.f;
  if(std::isnan(number))return Value::make_int(0);
  if(number>=std::numeric_limits<int32_t>::max())return Value::make_int(INT32_MAX);
  if(number<=std::numeric_limits<int32_t>::min())return Value::make_int(INT32_MIN);
  return Value::make_int(static_cast<int32_t>(number));
}
Value empty_method_result(const std::string& desc) {
  auto p=desc.find(')');if(p==std::string::npos||p+1>=desc.size())return Value::make_void();
  char t=desc[p+1];if(t=='L'||t=='[')return Value::make_obj(nullptr);
  if(t=='F'||t=='D')return Value::make_float(0);if(t!='V')return Value::make_int(0);return Value::make_void();
}

bool file_readable(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in);
}

constexpr uint32_t kNativeFlag = 0x40;

// String[] has no class file. MultiChoice.setValue calls size/elementAt on
// the choice list; when that list is still the array, those calls must
// return the slot or the following changeText underflows.
Value array_call(Object* recv, const std::string& method,
                  const std::vector<Value>& args) {
  if (!recv || !recv->is_array) return Value::make_void();
  if (method == "size" || method == "length")
    return Value::make_int(static_cast<int32_t>(recv->slots.size()));
  if (method == "elementAt" || method == "get") {
    int32_t idx = 0;
    if (!args.empty()) {
      if (args[0].tag == ValueTag::Int)
        idx = args[0].v.i;
      else if (args[0].tag == ValueTag::Float)
        idx = static_cast<int32_t>(args[0].v.f);
    }
    if (idx >= 0 && static_cast<size_t>(idx) < recv->slots.size())
      return recv->slots[static_cast<size_t>(idx)];
    return Value::make_obj(nullptr);
  }
  return Value::make_void();
}

float bits_to_float_u32(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

}  // namespace

Jvm::Jvm(NativeRegistry* natives) : natives_(natives) {}

void Jvm::halt_unbound(const std::string& owner, const std::string& method,
                       const std::string& jni) {
  if (unbound_.empty()) {
    unbound_ = owner + "." + method + " " + jni;
    std::fprintf(stderr, "[jvm] unbound native %s\n", unbound_.c_str());
    std::fflush(stderr);
  }
  throw std::runtime_error(unbound_);
}

void Jvm::set_game_root(std::string root) { game_root_ = std::move(root); }

bool Jvm::bootstrap_classpath() {
  if (game_root_.empty()) {
    std::fprintf(stderr, "[jvm] bootstrap: empty game_root\n");
    return false;
  }
  classpath_ = {
      {"system/Scripts/lang", "java.lang"},
      {"system/Scripts/io", "java.io"},
      {"system/Scripts/util", "java.util"},
      {"system/Scripts/util/resource", "java.util.resource"},
      {"system/Scripts/render", "java.render"},
      {"system/Scripts/render/osd", "java.render.osd"},
      {"system/Scripts/render/osd/dialog", "java.render.osd.dialog"},
      {"system/Scripts/sound", "java.sound"},
      {"sl/Scripts/game", "java.game"},
      {"parts/scripts", "java.game.parts"},
      {"parts/scripts/bodypart", "java.game.parts.bodypart"},
      {"parts/accessories/scripts", "java.game.parts.accessories"},
  };
  // Engine/running-gear base classes live below parts/scripts. Resource
  // classes in individual car packs add their own directory when loaded.
  std::error_code directory_error;
  const std::filesystem::path parts_root=std::filesystem::path(game_root_)/"parts/scripts";
  if(std::filesystem::is_directory(parts_root,directory_error))
    for(const auto& entry:std::filesystem::recursive_directory_iterator(parts_root))if(entry.is_directory()) {
      auto suffix=entry.path().lexically_relative(parts_root).generic_string();
      std::string package="java.game.parts."+suffix;std::replace(package.begin(),package.end(),'/','.');
      classpath_.push_back({"parts/scripts/"+suffix,package});
    }
  classpath_ready_ = true;
  std::printf("[jvm] classpath ready (%zu roots) under %s\n", classpath_.size(),
              game_root_.c_str());
  return true;
}

std::string Jvm::resolve_class_path(const std::string& fqn) const {
  const auto slash = fqn.find_last_of('.');
  const std::string simple =
      slash == std::string::npos ? fqn : fqn.substr(slash + 1);
  const std::string pkg =
      slash == std::string::npos ? std::string() : fqn.substr(0, slash);

  for (const auto& root : classpath_) {
    if (!pkg.empty() && root.second != pkg) continue;
    const std::string path = (std::filesystem::path(game_root_)/root.first/(simple+".class")).string();
    if (file_readable(path)) return path;
  }
  for (const auto& root : classpath_) {
    const std::string path = (std::filesystem::path(game_root_)/root.first/(simple+".class")).string();
    if (file_readable(path)) return path;
  }
  return {};
}

void Jvm::seed_static_fields(const TufaClass& cls) {
  // PE Class_ensureInitialized runs each static field's init TREE. Seed only
  // simple literal trees (`static final int CMD_* = N`) — skip new/call bodies
  // so we don't overwrite host-set object statics with a stray argc int.
  const std::string owner = cls.name();
  if (owner.empty()) return;
  for (const auto& f : cls.static_fields) {
    const std::string fname = cls.utf8(f.name);
    if (fname.empty()) continue;
    if (f.tree == kTufaNone || f.tree >= cls.trees.size()) continue;
    const auto& tree = cls.trees[f.tree];
    if (tree.empty() || tree.size() > 8) continue;
    Value lit = Value::make_void();
    bool complex = false;
    bool negate_next = false;
    for (const auto& n : tree) {
      // Allow: expr/literal markers, int/float/rid/null, end/ret.
      // Unary neg is an expr tag (imm 22). `-1.2` is a positive literal
      // plus that tag; applying it keeps CREDITS_TOP negative.
      if (n.op == 0x07 && n.wide && n.imm == 22) {
        if (lit.tag == ValueTag::Float)
          lit.v.f = -lit.v.f;
        else if (lit.tag == ValueTag::Int)
          lit.v.i = -lit.v.i;
        else
          negate_next = true;
      } else if (n.op == 0x0b && n.wide) {  // int
        lit = Value::make_int(static_cast<int32_t>(n.imm));
        if (negate_next) {
          lit.v.i = -lit.v.i;
          negate_next = false;
        }
      } else if (n.op == 0x0a && n.wide) {  // float
        lit = Value::make_float(bits_to_float_u32(n.imm));
        if (negate_next) {
          lit.v.f = -lit.v.f;
          negate_next = false;
        }
      } else if (n.op == 0x0e && n.wide) {
        // imm = CONS Resource index, not the local typeid (same as TREE rid).
        try {
          const auto rid = cls.resource(n.imm);
          lit = Value::make_int(static_cast<int32_t>(
              rpk_resolve_resource(rid.pack, rid.id)));
        } catch (...) {
          lit = Value::make_int(0);
        }
      }
      else if (n.op == 0x0c)  // null
        lit = Value::make_obj(nullptr);
      else if (n.op == 0x09 && n.wide)  // string literal (Config.version)
        lit = Value::make_obj(new_string(cls.utf8(n.imm)));
      else if (n.op == 0x07 || n.op == 0x10 || n.op == 0x2a || n.op == 0x2b ||
               n.op == 0x29)
        continue;
      else {
        complex = true;
        break;
      }
    }
    if (complex || lit.tag == ValueTag::Void) continue;
    statics_[owner][fname] = lit;
  }
}

namespace {
bool tree_has_call(const std::vector<TufaNode>& tree) {
  for (const auto& n : tree) {
    if (n.op == 0x24 || n.op == 0x25 || n.op == 0x26) return true;
  }
  return false;
}
}  // namespace

void Jvm::enrich_static_fields(const TufaClass& cls) {
  // Literal seeding keeps the last float of `4.0/3.0` and skips
  // `new ResourceRef(...)`. Re-run those trees once the class is registered.
  const std::string owner = cls.name();
  if (owner.empty() || field_init_depth_ > 6) return;
  ++field_init_depth_;
  for (const auto& f : cls.static_fields) {
    if (f.tree == kTufaNone || f.tree >= cls.trees.size()) continue;
    const auto& tree = cls.trees[f.tree];
    if (tree.empty() || tree.size() > 48) continue;
    bool has_new = false;
    for (const auto& n : tree)
      if (n.op == 0x21) has_new = true;
    const bool call = tree_has_call(tree);
    // `new ResourceRef(MW_Mod:0xABC)` is a call. Skip other calls so a stray
    // argc int cannot overwrite a host static. Keep the constructed object.
    if (call && !has_new) continue;
    bool rich = has_new;
    for (const auto& n : tree) {
      if (n.op == 0x07 && n.wide && n.imm >= 15 && n.imm <= 22) rich = true;
      // `new String[n]` (CLUBNAMES, speedymen). Expr 31 is the array alloc.
      if (n.op == 0x07 && n.wide && n.imm == 31) rich = true;
    }
    if (!rich) continue;
    const std::string fname = cls.utf8(f.name);
    if (fname.empty()) continue;
    TufaMember m = f;
    if (m.locals < 1) m.locals = 1;
    TreeEvalResult tr = tree_eval(this, cls, m, nullptr, {});
    if (!tr.ok || tr.value.tag == ValueTag::Void) continue;
    if (call && tr.value.tag != ValueTag::Obj) continue;
    statics_[owner][fname] = tr.value;
  }
  --field_init_depth_;
}

void Jvm::apply_field_defaults(Object* obj) {
  if (!obj || field_init_depth_ > 6) return;
  ++field_init_depth_;
  std::vector<const TufaClass*> chain;
  std::string cur = obj->class_name;
  for (int i = 0; i < 24 && !cur.empty(); ++i) {
    const TufaClass* c = find_class(cur);
    // MainMenuDialog is constructed before OptionsDialog is loaded.
    // Field trees (Style butt0 / Menu_Left) live on the superclass; skipping
    // them left every options page with a null style.
    if (!c) {
      load_class(cur);
      c = find_class(cur);
    }
    if (!c) break;
    chain.push_back(c);
    cur = c->super_name();
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const TufaClass* c = *it;
    for (const auto& f : c->instance_fields) {
      if (f.tree == kTufaNone || f.tree >= c->trees.size()) continue;
      const auto& tree = c->trees[f.tree];
      // Keep call trees. `actId = startId + Math.random()*(n)` is the
      // character id; skipping it left the portrait on resource 0.
      if (tree.empty() || tree.size() > 48) continue;
      const std::string fname = c->utf8(f.name);
      if (fname.empty()) continue;
      const std::string desc = c->utf8(f.descriptor);
      const bool log_fail = desc.find("Style") != std::string::npos;
      TufaMember m = f;
      m.flags &= ~0x08u;
      if (m.locals < 1) m.locals = 1;
      TreeEvalResult tr;
      try {
        tr = tree_eval(this, *c, m, obj, {});
      } catch (const std::exception& e) {
        if (log_fail)
          std::fprintf(stderr, "[jvm] field init %s.%s: %s\n",
                       c->name().c_str(), fname.c_str(), e.what());
        continue;
      }
      if (!tr.ok || tr.value.tag == ValueTag::Void) {
        if (log_fail && !tr.ok)
          std::fprintf(stderr, "[jvm] field init %s.%s: %s\n",
                       c->name().c_str(), fname.c_str(), tr.error.c_str());
        continue;
      }
      Value stored = tr.value;
      if (desc == "I" && stored.tag == ValueTag::Float)
        stored = Value::make_int(static_cast<int32_t>(stored.v.f));
      else if (desc == "F" && stored.tag == ValueTag::Int)
        stored = Value::make_float(static_cast<float>(stored.v.i));
      obj->fields[fname] = stored;
    }
  }
  --field_init_depth_;
}

bool Jvm::load_class_file(const std::string& path) {
  if (!loading_paths_.insert(path).second) return true;
  struct LoadingGuard {
    std::unordered_set<std::string>* set;
    std::string path;
    ~LoadingGuard() { set->erase(path); }
  } guard{&loading_paths_, path};

  std::vector<TufaClass> blobs;
  std::string err;
  if (!tufa_load_file(path, &blobs, &err)) {
    std::fprintf(stderr, "[jvm] TUFA load failed %s: %s\n", path.c_str(),
                 err.c_str());
    return false;
  }
  for (auto& cls : blobs) {
    const std::string name = cls.name();
    auto dot=name.find_last_of('.');
    if(dot!=std::string::npos){
      auto directory=std::filesystem::absolute(path).parent_path().string();
      std::string package=name.substr(0,dot);
      auto candidate=std::make_pair(directory,package);
      if(std::find(classpath_.begin(),classpath_.end(),candidate)==classpath_.end())classpath_.push_back(candidate);
    }
    std::printf(
        "[jvm] loaded %s  build=0x%x  fields=%zu/%zu  methods=%zu/%zu  "
        "trees=%zu  from %s\n",
        name.c_str(), cls.build, cls.static_fields.size(),
        cls.instance_fields.size(), cls.static_methods.size(),
        cls.instance_methods.size(), cls.trees.size(), path.c_str());
    seed_static_fields(cls);
    by_name_[name] = std::move(cls);
    enrich_static_fields(by_name_[name]);
  }
  return !blobs.empty();
}

bool Jvm::load_class(const std::string& fqn_or_path) {
  if (fqn_or_path.size() >= 6 &&
      fqn_or_path.compare(fqn_or_path.size() - 6, 6, ".class") == 0)
    return load_class_file(fqn_or_path);

  if (!classpath_ready_) {
    std::fprintf(stderr, "[jvm] load_class: classpath not ready\n");
    return false;
  }
  if (find_class(fqn_or_path)) return true;

  const std::string path = resolve_class_path(fqn_or_path);
  if (path.empty()) {
    std::fprintf(stderr, "[jvm] class not found: %s\n", fqn_or_path.c_str());
    return false;
  }
  if (!load_class_file(path)) return false;
  return find_class(fqn_or_path) != nullptr;
}

const TufaClass* Jvm::find_class(const std::string& fqn) const {
  auto it = by_name_.find(fqn);
  if (it != by_name_.end()) return &it->second;
  const auto slash = fqn.find_last_of('.');
  if (slash != std::string::npos) {
    it = by_name_.find(fqn.substr(slash + 1));
    if (it != by_name_.end()) return &it->second;
  } else {
    // Field refs sometimes name the class without its package. Reloading
    // that short name re-runs static init and recurses.
    const std::string suffix = "." + fqn;
    for (const auto& kv : by_name_) {
      if (kv.first.size() >= suffix.size() &&
          kv.first.compare(kv.first.size() - suffix.size(), suffix.size(),
                           suffix) == 0)
        return &kv.second;
    }
  }
  return nullptr;
}

Object* Jvm::new_object(const std::string& class_fqn) {
  if (!find_class(class_fqn)) load_class(class_fqn);
  auto obj = std::make_unique<Object>();
  obj->class_name = class_fqn;
  Object* raw = obj.get();
  heap_.push_back(std::move(obj));
  apply_field_defaults(raw);
  return raw;
}

int Jvm::tree_field_kind(const std::string& owner, const std::string& field) {
  // `[T.length` is stored on the array object, not on a class.
  if (owner.empty() || field.empty() || owner[0] == '[') return 0;
  if (!find_class(owner)) load_class(owner);
  std::string cur = owner;
  for (int depth = 0; depth < 48 && !cur.empty(); ++depth) {
    const TufaClass* c = find_class(cur);
    if (!c) break;
    for (const auto& sf : c->static_fields)
      if (c->utf8(sf.name) == field) return 1;
    for (const auto& inf : c->instance_fields)
      if (c->utf8(inf.name) == field) return 2;
    const std::string next = c->super_name();
    if (next.empty() || next == cur) break;
    cur = next;
  }
  return 0;
}

bool Jvm::tree_instance_of(Object* obj, const std::string& type) {
  if (!obj || type.empty() || obj->class_name.empty()) return false;
  std::string cur = obj->class_name;
  for (int i = 0; i < 24 && !cur.empty(); ++i) {
    if (cur == type) return true;
    const TufaClass* c = find_class(cur);
    if (!c) {
      if (!load_class(cur)) return false;
      c = find_class(cur);
      if (!c) return false;
    }
    std::string next = c->super_name();
    if (next.empty() || next == cur) return false;
    cur = std::move(next);
  }
  return false;
}

Object* Jvm::new_string(const std::string& utf8) {
  Object* o = new_object("java.lang.String");
  o->str = utf8;
  return o;
}

int Jvm::count_args(const std::string& jni_sig) {
  if (jni_sig.empty() || jni_sig[0] != '(') return -1;
  int n = 0;
  for (size_t i = 1; i < jni_sig.size() && jni_sig[i] != ')';) {
    const char c = jni_sig[i];
    if (c == 'L') {
      while (i < jni_sig.size() && jni_sig[i] != ';') ++i;
      if (i < jni_sig.size()) ++i;  // consume ';'
      ++n;
    } else if (c == '[') {
      while (i < jni_sig.size() && jni_sig[i] == '[') ++i;
      if (i < jni_sig.size() && jni_sig[i] == 'L') {
        while (i < jni_sig.size() && jni_sig[i] != ';') ++i;
        if (i < jni_sig.size()) ++i;
      } else if (i < jni_sig.size()) {
        ++i;  // primitive array element
      }
      ++n;
    } else {
      ++i;
      ++n;
    }
  }
  return n;
}

// Soft TUFA often stores ctor/method descriptors as "(…)V" or bare "(…)" /
// "(…)". Compare arg lists; treat missing return as void.
bool descriptor_match(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto norm = [](std::string s) {
    if (s.empty() || s[0] != '(') return s;
    const auto rp = s.find(')');
    if (rp == std::string::npos) return s;
    if (rp + 1 >= s.size()) s.push_back('V');
    return s;
  };
  return norm(a) == norm(b);
}

const TufaMember* Jvm::find_method(const TufaClass& cls,
                                   const std::string& method,
                                   const std::string& jni_sig,
                                   bool prefer_static) const {
  auto scan = [&](const std::vector<TufaMember>& methods) -> const TufaMember* {
    for (const auto& m : methods) {
      if (cls.utf8(m.name) == method &&
          descriptor_match(cls.utf8(m.descriptor), jni_sig))
        return &m;
    }
    return nullptr;
  };
  if (prefer_static) {
    if (const TufaMember* m = scan(cls.static_methods)) return m;
    return scan(cls.instance_methods);
  }
  if (const TufaMember* m = scan(cls.instance_methods)) return m;
  return scan(cls.static_methods);
}

const TufaMember* Jvm::find_method_by_name(
    const TufaClass& cls, const std::string& method, int argc,
    bool prefer_static, const std::vector<Value>* runtime, int* match_score) const {
  if(match_score)*match_score=-100000;
  // Two createButton overloads share argc. Match the live tags
  // (String,int) against (int,String) instead of taking the first one.
  auto score = [&](const std::string& jni) -> int {
    if (!runtime || jni.empty() || jni[0] != '(') return 0;
    int s = 0;
    size_t i = 1;
    size_t ai = 0;
    while (i < jni.size() && jni[i] != ')') {
      if (ai >= runtime->size()) return -1000;
      const Value& a = (*runtime)[ai];
      char c = jni[i];
      if (c == '[') {
        while (i < jni.size() && jni[i] == '[') ++i;
        c = i < jni.size() ? jni[i] : 0;
      }
      if (c == 'L' || jni[i] == 'L') {
        const size_t begin = (i < jni.size() && jni[i] == 'L') ? i + 1 : i;
        while (i < jni.size() && jni[i] != ';') ++i;
        std::string typ = jni.substr(begin, i - begin);
        for (char& ch : typ)
          if (ch == '/') ch = '.';
        if (i < jni.size()) ++i;
        // Any object used to score the same, so File.write(ResourceRef)
        // (declared before write(String)) won and stored player.name as
        // id 0. Load then read a zero length and the header became "null".
        if (a.tag != ValueTag::Obj || !a.v.o)
          s -= 4;
        else if (!typ.empty() &&
                 const_cast<Jvm*>(this)->tree_instance_of(a.v.o, typ))
          s += 8;
        else
          s += 1;
      } else if (c == 'F' || c == 'D') {
        ++i;
        if (a.tag == ValueTag::Float) s += 3;
        else if (a.tag == ValueTag::Int) s += 1;
        else s -= 4;
      } else if (c == 'I' || c == 'Z' || c == 'B' || c == 'S' || c == 'C' ||
                 c == 'J') {
        ++i;
        if (a.tag == ValueTag::Int) s += 3;
        else if (a.tag == ValueTag::Float) s += 0;
        else s -= 4;
      } else {
        ++i;
      }
      ++ai;
    }
    if (ai != runtime->size()) return -1000;
    return s;
  };
  auto scan = [&](const std::vector<TufaMember>& methods) -> const TufaMember* {
    const TufaMember* fallback = nullptr;
    const TufaMember* best = nullptr;
    int best_score = -100000;
    for (const auto& m : methods) {
      if (cls.utf8(m.name) != method) continue;
      if (argc < 0) return &m;
      const std::string desc = cls.utf8(m.descriptor);
      const int n = count_args(desc);
      if (n != argc) {
        // Constructors are not inherited and must match arity. A 5-arg
        // super() on a 4-arg <init> used to shift every argument.
        if (!fallback && method != "<init>") fallback = &m;
        continue;
      }
      const int sc = score(desc);
      if (!best || sc > best_score) {
        best = &m;
        best_score = sc;
      }
    }
    if(match_score&&(best||fallback))*match_score=best?best_score:-10000;
    return best ? best : fallback;
  };
  if (prefer_static) {
    if (const TufaMember* m = scan(cls.static_methods)) return m;
    return scan(cls.instance_methods);
  }
  if (const TufaMember* m = scan(cls.instance_methods)) return m;
  return scan(cls.static_methods);
}

bool Jvm::native_has_alias(const std::string& class_name,
                           const std::string& method,
                           const std::string& jni_sig,
                           std::string* out_registry_key) const {
  auto hit = [&](const std::string& key) {
    if (!natives_->has(key, method, jni_sig)) return false;
    if (out_registry_key) *out_registry_key = key;
    return true;
  };
  if (hit(class_name)) return true;
  const auto dot = class_name.find_last_of('.');
  if (dot == std::string::npos) {
    // Short TUFA name → probe classpath packages (GameType → java.lang.GameType).
    for (const auto& root : classpath_) {
      if (hit(root.second + "." + class_name)) return true;
    }
  } else if (hit(class_name.substr(dot + 1))) {
    return true;
  }
  return false;
}

bool Jvm::find_native_owner(const std::string& class_fqn,
                            const std::string& method,
                            const std::string& jni_sig,
                            std::string* out_owner_fqn) const {
  // Stock PE: native table is per declaring class; calls on SplashScreen
  // resolve GameType.setEventMask etc. by walking super.
  std::string cur = class_fqn;
  for (int depth = 0; depth < 48 && !cur.empty(); ++depth) {
    if (native_has_alias(cur, method, jni_sig, out_owner_fqn)) return true;
    const_cast<Jvm*>(this)->load_class(cur);
    const TufaClass* c = find_class(cur);
    if (!c) break;
    cur = c->super_name();
  }
  return false;
}

const TufaMember* Jvm::find_method_inherited(const std::string& class_fqn,
                                             const std::string& method,
                                             const std::string& jni_sig,
                                             bool prefer_static,
                                             std::string* out_owner_fqn) const {
  std::string cur = class_fqn;
  for (int depth = 0; depth < 48 && !cur.empty(); ++depth) {
    const_cast<Jvm*>(this)->load_class(cur);
    const TufaClass* c = find_class(cur);
    if (!c) break;
    if (const TufaMember* m = find_method(*c, method, jni_sig, prefer_static)) {
      if (out_owner_fqn) *out_owner_fqn = cur;
      return m;
    }
    cur = c->super_name();
  }
  return nullptr;
}

Value Jvm::get_static(const std::string& class_fqn, const std::string& field) {
  // operator[] would insert a Void and hide the real initializer. Racer.RID_FEJ
  // is only seeded when the class loads; the portrait dialog reads it first.
  if (!class_fqn.empty() && !find_class(class_fqn)) load_class(class_fqn);
  auto cit = statics_.find(class_fqn);
  if (cit == statics_.end()) return Value::make_void();
  auto fit = cit->second.find(field);
  if (fit == cit->second.end()) return Value::make_void();
  if (fit->second.tag == ValueTag::Int && (fit->second.v.i & 0xffff0000) == 0) {
    if (const auto* cls = find_class(class_fqn)) for (const auto& f : cls->static_fields) {
      if (cls->utf8(f.name) != field || f.tree == kTufaNone || f.tree >= cls->trees.size()) continue;
      const auto& tree = cls->trees[f.tree]; if (tree.size() > 8) break;
      for (const auto& n : tree) if(n.op == 0x0e && n.wide) {
        const auto rid=cls->resource(n.imm);
        if(static_cast<uint32_t>(fit->second.v.i)==rid.id)
          fit->second=Value::make_int(static_cast<int32_t>(rpk_resolve_resource(rid.pack,rid.id)));
      }
      break;
    }
  }
  return fit->second;
}

void Jvm::set_static(const std::string& class_fqn, const std::string& field,
                     Value v) {
  statics_[class_fqn][field] = v;
}

Object* Jvm::tree_new(const std::string& class_fqn) {
  return new_object(class_fqn);
}

Value Jvm::tree_call(const std::string& class_fqn, const std::string& method,
                     const std::string& jni_sig, Object* self,
                     const std::vector<Value>& args, bool is_static) {
  std::vector<Value> full = args;
  if (!is_static && self)
    full.insert(full.begin(), Value::make_obj(self));
  return invoke(class_fqn, method, jni_sig, full, is_static);
}

Value Jvm::tree_call_by_name(const std::string& class_fqn,
                             const std::string& method,
                             const std::vector<Value>& args,
                             bool prefer_static) {
  return invoke_by_name(class_fqn, method, args, prefer_static);
}

Value Jvm::tree_get_static(const std::string& class_fqn,
                           const std::string& field) {
  return get_static(class_fqn, field);
}

void Jvm::tree_set_static(const std::string& class_fqn,
                          const std::string& field, Value v) {
  set_static(class_fqn, field, v);
}

Object* Jvm::tree_new_string(const std::string& utf8) {
  return new_string(utf8);
}

Value Jvm::invoke_by_name(const std::string& class_fqn,
                          const std::string& method,
                          const std::vector<Value>& args, bool prefer_static) {
  if (!find_class(class_fqn)) load_class(class_fqn);
  const TufaClass* cls = find_class(class_fqn);
  if (!cls) {
    if (!prefer_static && !args.empty() && args[0].tag == ValueTag::Obj &&
        args[0].v.o && args[0].v.o->is_array) {
      std::vector<Value> rest(args.begin() + 1, args.end());
      Value hit = array_call(args[0].v.o, method, rest);
      if (hit.tag != ValueTag::Void || method == "elementAt" ||
          method == "size" || method == "get" || method == "length")
        return hit;
    }
    std::fprintf(stderr, "[jvm] invoke_by_name: class %s\n", class_fqn.c_str());
    return Value::make_void();
  }
  // Instance args include receiver at [0].
  int argc = static_cast<int>(args.size());
  Object* self = nullptr;
  std::vector<Value> call_args = args;
  bool is_static = prefer_static;
  if (!prefer_static && !call_args.empty() &&
      call_args[0].tag == ValueTag::Obj) {
    self = call_args[0].v.o;
    call_args.erase(call_args.begin());
    argc = static_cast<int>(call_args.size());
    is_static = false;
  }

  // Soft: resolve most-specific method first. Superclass host leaves must not
  // shadow a subclass Java override (Dialog.show void0 ≠ MainMenuDialog.show).
  std::string owner = class_fqn;
  const TufaClass* owner_cls = cls;
  int best_score=-100000;
  const TufaMember* m =
      find_method_by_name(*cls, method, argc, is_static, &call_args, &best_score);
  if (method != "<init>" || !m) {
    std::string cur = cls->super_name();
    for (int depth = 0; depth < 48 && !cur.empty(); ++depth) {
      load_class(cur);
      const TufaClass* c = find_class(cur);
      if (!c) break;
      int candidate_score=-100000;
      const auto* candidate=find_method_by_name(*c,method,argc,is_static,&call_args,&candidate_score);
      if (candidate && (!m||candidate_score>best_score)) {
        m=candidate;best_score=candidate_score;
        owner = cur;
        owner_cls = c;
        // Invictus inherits resource constructors; choose the nearest one.
        if(method=="<init>")break;
      }
      cur = c->super_name();
    }
  }
  if (!m) {
    if (method == "<init>") return Value::make_void();
    // Dialog.show calls osd.create*; a bare name resolves on the dialog
    // class and misses. The methods live on java.render.Osd.
    const bool osd_build =
        method == "createMenu" || method == "createButton" ||
        method == "createText" || method == "createRectangle" ||
        method == "createHeader" || method == "createHotkey" ||
        method == "createBG" || method == "createTextBox" ||
        method == "createStringInput" || method == "endGroup" ||
        method == "hideGroup" || method == "showGroup" ||
        method == "beginGroup";
    if (osd_build && class_fqn != "java.render.Osd") {
      Object* osd = nullptr;
      auto take_osd = [](Object* o) {
        if (!o) return false;
        const std::string& n = o->class_name;
        return n.size() >= 4 && n.compare(n.size() - 4, 4, ".Osd") == 0;
      };
      if (take_osd(self)) osd = self;
      if (!osd && self) {
        auto it = self->fields.find("osd");
        if (it != self->fields.end() && it->second.tag == ValueTag::Obj &&
            take_osd(it->second.v.o))
          osd = it->second.v.o;
      }
      if (!osd) {
        for (const Value& a : call_args) {
          if (a.tag == ValueTag::Obj && take_osd(a.v.o)) {
            osd = a.v.o;
            break;
          }
        }
      }
      if (osd) {
        std::vector<Value> again = call_args;
        again.insert(again.begin(), Value::make_obj(osd));
        return invoke_by_name("java.render.Osd", method, again, false);
      }
    }
    std::fprintf(stderr, "[jvm] invoke_by_name miss %s.%s argc=%d\n",
                 class_fqn.c_str(), method.c_str(), argc);
    return Value::make_void();
  }
  const std::string jni = owner_cls->utf8(m->descriptor);
  if (method == "<init>" && owner != class_fqn) {
    std::printf("[jvm] ctor %s argc=%d -> %s %s\n", class_fqn.c_str(), argc,
                owner.c_str(), jni.c_str());
  }
  const bool empty_tree =
      m->tree == kTufaNone || m->tree >= owner_cls->trees.size() ||
      owner_cls->trees[m->tree].empty();
  const bool is_native = (m->flags & kNativeFlag) != 0;

  std::string host_key;
  // Host leaf on declaring class (GameLogic.changeActiveSection, WIP Osd.*).
  if (native_has_alias(owner, method, jni, &host_key))
    return natives_->invoke(host_key, method, jni, self, call_args);

  if (is_native || empty_tree) {
    if(empty_tree&&!is_native)return empty_method_result(jni);
    if (method == "<init>") return Value::make_void();
    if (halt_on_unbound_ && is_native) halt_unbound(owner, method, jni);
    std::fprintf(stderr,
                 "[jvm] native/empty TREE without host binding %s.%s %s "
                 "(from %s)\n",
                 owner.c_str(), method.c_str(), jni.c_str(), class_fqn.c_str());
    return Value::make_void();
  }

  if (invoke_depth_ > 64) {
    static int logged = 0;
    if (logged < 3) {
      ++logged;
      std::fprintf(stderr, "[jvm] invoke depth exceeded %s.%s\n",
                   owner.c_str(), method.c_str());
    }
    return Value::make_void();
  }
  ++invoke_depth_;
  TreeEvalResult tr;
  try {
    tr = tree_eval(this, *owner_cls, *m, self, call_args);
  } catch (const std::exception& e) {
    --invoke_depth_;
    if (halt_on_unbound_ && !unbound_.empty()) {
      if (invoke_depth_ > 0) throw;
      return Value::make_void();
    }
    std::fprintf(stderr, "[jvm] TREE exception %s.%s: %s\n", owner.c_str(),
                 method.c_str(), e.what());
    return Value::make_void();
  }
  --invoke_depth_;
  if (!tr.ok) {
    std::fprintf(stderr, "[jvm] TREE fail %s\n", tr.error.c_str());
    std::fflush(stderr);
    if (halt_on_unbound_) {
      if (unbound_.empty()) unbound_ = std::string("TREE ") + tr.error;
      if (invoke_depth_ > 0) throw std::runtime_error(unbound_);
    }
    return Value::make_void();
  }
  return declared_return(tr.value, owner_cls->utf8(m->descriptor));
}

Value Jvm::invoke(const std::string& class_fqn, const std::string& method,
                  const std::string& jni_sig, const std::vector<Value>& args,
                  bool is_static) {
  Object* self = nullptr;
  std::vector<Value> call_args = args;
  if (!is_static && !call_args.empty() && call_args[0].tag == ValueTag::Obj) {
    self = call_args[0].v.o;
    call_args.erase(call_args.begin());
  }

  if (!find_class(class_fqn)) load_class(class_fqn);
  const TufaClass* cls = find_class(class_fqn);
  if (!cls) {
    if (self && self->is_array) {
      Value hit = array_call(self, method, call_args);
      if (hit.tag != ValueTag::Void || method == "elementAt" ||
          method == "size" || method == "get" || method == "length")
        return hit;
    }
    std::fprintf(stderr, "[jvm] invoke: class not loaded %s\n",
                 class_fqn.c_str());
    return Value::make_void();
  }

  // Soft PE: resolve most-specific method, then native vs TREE on that owner.
  // Never let a superclass host leaf (Dialog.show) shadow a subclass override.
  std::string owner;
  const TufaMember* m =
      find_method_inherited(class_fqn, method, jni_sig, is_static, &owner);
  if (!m) {
    std::fprintf(stderr, "[jvm] invoke miss %s.%s %s\n", class_fqn.c_str(),
                 method.c_str(), jni_sig.c_str());
    return Value::make_void();
  }
  const TufaClass* owner_cls = find_class(owner);
  if (!owner_cls) owner_cls = cls;

  const bool empty_tree =
      m->tree == kTufaNone || m->tree >= owner_cls->trees.size() ||
      owner_cls->trees[m->tree].empty();
  const bool is_native = (m->flags & kNativeFlag) != 0;

  std::string host_key;
  if (is_native || empty_tree) {
    if (native_has_alias(owner, method, jni_sig, &host_key))
      return natives_->invoke(host_key, method, jni_sig, self, call_args);
    if(empty_tree&&!is_native)return empty_method_result(jni_sig);
    if (halt_on_unbound_ && is_native) halt_unbound(owner, method, jni_sig);
    std::fprintf(stderr,
                 "[jvm] native/empty TREE without host binding %s.%s %s "
                 "(from %s)\n",
                 owner.c_str(), method.c_str(), jni_sig.c_str(),
                 class_fqn.c_str());
    return Value::make_void();
  }

  // Java body — same-class host leaf still allowed (WIP Osd.createButton…).
  if (native_has_alias(owner, method, jni_sig, &host_key))
    return natives_->invoke(host_key, method, jni_sig, self, call_args);

  if (invoke_depth_ > 64) {
    static int logged = 0;
    if (logged < 3) {
      ++logged;
      std::fprintf(stderr, "[jvm] invoke depth exceeded %s.%s\n",
                   class_fqn.c_str(), method.c_str());
    }
    return Value::make_void();
  }
  ++invoke_depth_;
  TreeEvalResult tr;
  try {
    tr = tree_eval(this, *owner_cls, *m, self, call_args);
  } catch (const std::exception& e) {
    --invoke_depth_;
    if (halt_on_unbound_ && !unbound_.empty()) {
      if (invoke_depth_ > 0) throw;
      return Value::make_void();
    }
    std::fprintf(stderr, "[jvm] TREE exception %s.%s: %s\n", owner.c_str(),
                 method.c_str(), e.what());
    std::fflush(stderr);
    return Value::make_void();
  }
  --invoke_depth_;
  if (!tr.ok) {
    std::fprintf(stderr, "[jvm] TREE fail %s\n", tr.error.c_str());
    std::fflush(stderr);
    if (halt_on_unbound_) {
      if (unbound_.empty()) unbound_ = std::string("TREE ") + tr.error;
      if (invoke_depth_ > 0) throw std::runtime_error(unbound_);
    }
    return Value::make_void();
  }
  return declared_return(tr.value, owner_cls->utf8(m->descriptor));
}

}  // namespace slrr

