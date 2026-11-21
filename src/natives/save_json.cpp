#include "save_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace slrr::save_json {
namespace {

constexpr int32_t kCareerId = static_cast<int32_t>(0x87654321u);   // GameLogic.SAVEFILEID_MAIN
constexpr int32_t kVehicleId = static_cast<int32_t>(0x88664422u);  // Vehicle.SAVEFILEID_VHC
constexpr int32_t kSkinId = static_cast<int32_t>(0x82549867u);     // Vehicle.SAVEFILEID_SKIN
constexpr int kMaxDepth = 512;

// ---------------------------------------------------------------- JSON DOM

struct Json {
  enum Type : uint8_t { Null, Bool, Number, String, Array, Object };
  Type type = Null;
  bool boolean = false;
  std::string text;  // number token, or string bytes
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  static Json number(std::string token) {
    Json j; j.type = Number; j.text = std::move(token); return j;
  }
  static Json string(std::string bytes) {
    Json j; j.type = String; j.text = std::move(bytes); return j;
  }
  static Json of(Type type) { Json j; j.type = type; return j; }
  const Json* get(std::string_view key) const {
    if (type != Object) return nullptr;
    for (const auto& member : members)
      if (member.first == key) return &member.second;
    return nullptr;
  }
  void add(std::string key, Json value) { members.emplace_back(std::move(key), std::move(value)); }
};

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}
  bool parse(Json& out) {
    if (text_.starts_with("\xef\xbb\xbf")) pos_ = 3;
    if (!value(out, 0)) return false;
    space();
    return pos_ == text_.size();
  }

 private:
  std::string_view text_;
  size_t pos_ = 0;

  void space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                                   text_[pos_] == '\r' || text_[pos_] == '\t'))
      ++pos_;
  }
  bool take(char c) {
    space();
    if (pos_ == text_.size() || text_[pos_] != c) return false;
    ++pos_;
    return true;
  }
  bool word(std::string_view w) {
    if (text_.substr(pos_, w.size()) != w) return false;
    pos_ += w.size();
    return true;
  }
  // Script strings are bytes: U+0001..U+00FF, escaped or as literal UTF-8.
  bool string(std::string& out) {
    out.clear();
    if (!take('"')) return false;
    while (pos_ < text_.size()) {
      unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') return true;
      if (c < 32) return false;
      if (c == '\\') {
        if (pos_ == text_.size()) return false;
        c = static_cast<unsigned char>(text_[pos_++]);
        switch (c) {
          case '"': case '\\': case '/': break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case 'u': {
            if (text_.size() - pos_ < 4) return false;
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const int h = hex_digit(text_[pos_++]);
              if (h < 0) return false;
              code = (code << 4) | static_cast<unsigned>(h);
            }
            if (code == 0 || code > 255) return false;
            c = static_cast<unsigned char>(code);
            break;
          }
          default: return false;
        }
      } else if (c >= 128) {
        if ((c != 0xc2 && c != 0xc3) || pos_ == text_.size()) return false;
        const unsigned char next = static_cast<unsigned char>(text_[pos_++]);
        if ((next & 0xc0) != 0x80) return false;
        c = static_cast<unsigned char>(((c & 3) << 6) | (next & 63));
      }
      out += static_cast<char>(c);
    }
    return false;
  }
  bool number(std::string& out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ == text_.size()) return false;
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      if (text_[pos_] < '1' || text_[pos_] > '9') return false;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      const size_t digits = ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (digits == pos_) return false;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      const size_t digits = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (digits == pos_) return false;
    }
    out.assign(text_.substr(start, pos_ - start));
    return true;
  }
  bool value(Json& out, int depth) {
    if (depth > kMaxDepth) return false;
    space();
    if (pos_ == text_.size()) return false;
    const char c = text_[pos_];
    if (c == '{') {
      ++pos_;
      out.type = Json::Object;
      if (take('}')) return true;
      do {
        std::string key;
        space();
        if (!string(key) || out.get(key) || !take(':')) return false;
        Json item;
        if (!value(item, depth + 1)) return false;
        out.add(std::move(key), std::move(item));
      } while (take(','));
      return take('}');
    }
    if (c == '[') {
      ++pos_;
      out.type = Json::Array;
      if (take(']')) return true;
      do {
        out.items.emplace_back();
        if (!value(out.items.back(), depth + 1)) return false;
      } while (take(','));
      return take(']');
    }
    if (c == '"') { out.type = Json::String; return string(out.text); }
    if (word("null")) { out.type = Json::Null; return true; }
    if (word("true")) { out.type = Json::Bool; out.boolean = true; return true; }
    if (word("false")) { out.type = Json::Bool; return true; }
    out.type = Json::Number;
    return number(out.text);
  }
};

void write_json(const Json& j, std::string& out, int indent);

bool is_container(const Json& j) { return j.type == Json::Array || j.type == Json::Object; }

// Containers of a few scalars stay on one line ({"rpk": ..., "id": ...}).
bool write_inline(const Json& j) {
  size_t length = 0;
  if (j.type == Json::Array) {
    for (const auto& item : j.items) {
      if (is_container(item)) return false;
      length += item.text.size() + 4;
    }
  } else {
    for (const auto& member : j.members) {
      if (is_container(member.second)) return false;
      length += member.first.size() + member.second.text.size() + 8;
    }
  }
  return length <= 72;
}

void write_json(const Json& j, std::string& out, int indent) {
  switch (j.type) {
    case Json::Null: out += "null"; return;
    case Json::Bool: out += j.boolean ? "true" : "false"; return;
    case Json::Number: out += j.text; return;
    case Json::String: out += json_string(j.text); return;
    default: break;
  }
  const bool array = j.type == Json::Array;
  const size_t count = array ? j.items.size() : j.members.size();
  out += array ? '[' : '{';
  if (count == 0) { out += array ? ']' : '}'; return; }
  const bool flat = write_inline(j);
  const std::string pad(static_cast<size_t>(indent + 2), ' ');
  for (size_t i = 0; i < count; ++i) {
    if (i) out += ',';
    if (flat) { if (i) out += ' '; }
    else { out += '\n'; out += pad; }
    if (array) {
      write_json(j.items[i], out, indent + 2);
    } else {
      out += json_string(j.members[i].first);
      out += ": ";
      write_json(j.members[i].second, out, indent + 2);
    }
  }
  if (!flat) { out += '\n'; out.append(static_cast<size_t>(indent), ' '); }
  out += array ? ']' : '}';
}

// ---------------------------------------------------------------- base64

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t n = (uint32_t(in[i]) << 16) |
        (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0u) |
        (i + 2 < in.size() ? uint32_t(in[i + 2]) : 0u);
    out += kBase64[(n >> 18) & 63];
    out += kBase64[(n >> 12) & 63];
    out += i + 1 < in.size() ? kBase64[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kBase64[n & 63] : '=';
  }
  return out;
}

bool base64_decode(std::string_view in, std::vector<uint8_t>& out) {
  out.clear();
  if (in.size() % 4) return false;
  for (size_t i = 0; i < in.size(); i += 4) {
    uint32_t n = 0;
    int pad = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char c = in[i + k];
      int v = -1;
      if (c == '=' && i + 4 == in.size() && k >= 2) { ++pad; v = 0; }
      else if (pad == 0) {
        const char* hit = std::strchr(kBase64, c);
        if (c && hit) v = static_cast<int>(hit - kBase64);
      }
      if (v < 0) return false;
      n = (n << 6) | static_cast<uint32_t>(v);
    }
    out.push_back(static_cast<uint8_t>(n >> 16));
    if (pad < 2) out.push_back(static_cast<uint8_t>(n >> 8));
    if (pad < 1) out.push_back(static_cast<uint8_t>(n));
  }
  return true;
}

// ---------------------------------------------------------------- leaves

std::string simple_name(std::string_view fqn) {
  const size_t dot = fqn.find_last_of("./");
  return std::string(dot == std::string_view::npos ? fqn : fqn.substr(dot + 1));
}

Json int_json(int32_t v) { return Json::number(std::to_string(v)); }

Json float_json(uint32_t bits) {
  float v;
  std::memcpy(&v, &bits, 4);
  if (!std::isfinite(v)) {
    Json o = Json::of(Json::Object);
    o.add("floatBits", Json::number(std::to_string(bits)));
    return o;
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
  std::string text(buffer, result.ptr);
  // A decimal point keeps floats distinguishable from ints in `values`.
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return Json::number(std::move(text));
}

Json resource_json(const Leaf& leaf, bool null_as_object) {
  Json o = Json::of(Json::Object);
  switch (leaf.res) {
    case ResKind::Null:
      if (!null_as_object) return Json{};
      o.add("rpk", Json{});
      return o;
    case ResKind::Pack: {
      std::string path = leaf.pack;
      std::replace(path.begin(), path.end(), '\\', '/');
      o.add("rpk", Json::string(std::move(path)));
      o.add("id", int_json(static_cast<int32_t>(leaf.local)));
      return o;
    }
    case ResKind::Blob:
      o.add("blob", int_json(leaf.blob));
      return o;
  }
  return o;
}

Json raw_json(const Leaf& leaf) {
  switch (leaf.type) {
    case LeafType::Int: return int_json(static_cast<int32_t>(leaf.word));
    case LeafType::Float: return float_json(leaf.word);
    case LeafType::String: return leaf.null_string ? Json{} : Json::string(leaf.text);
    case LeafType::Resource: return resource_json(leaf, true);
  }
  return Json{};
}

bool is_integer_token(const Json& j) {
  return j.type == Json::Number && j.text.find_first_of(".eE") == std::string::npos;
}

bool int64_of(const Json& j, int64_t& v, int64_t low, int64_t high) {
  if (!is_integer_token(j)) return false;
  const auto r = std::from_chars(j.text.data(), j.text.data() + j.text.size(), v);
  return r.ec == std::errc{} && r.ptr == j.text.data() + j.text.size() && v >= low && v <= high;
}

bool int_of(const Json& j, int32_t& v) {
  int64_t n = 0;
  if (!int64_of(j, n, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()))
    return false;
  v = static_cast<int32_t>(n);
  return true;
}

bool float_of(const Json& j, uint32_t& bits) {
  if (j.type == Json::Object) {
    const Json* raw = j.get("floatBits");
    int64_t n = 0;
    if (!raw || j.members.size() != 1 || !int64_of(*raw, n, 0, 0xffffffffLL)) return false;
    bits = static_cast<uint32_t>(n);
    return true;
  }
  if (j.type != Json::Number) return false;
  float v = 0;
  const auto r = std::from_chars(j.text.data(), j.text.data() + j.text.size(), v);
  if (r.ec != std::errc{} || r.ptr != j.text.data() + j.text.size() || !std::isfinite(v))
    return false;
  std::memcpy(&bits, &v, 4);
  return true;
}

bool resource_of(const Json& j, Leaf& leaf) {
  leaf = Leaf{};
  leaf.type = LeafType::Resource;
  if (j.type == Json::Null) return true;
  if (j.type != Json::Object) return false;
  if (const Json* blob = j.get("blob")) {
    int64_t n = 0;
    if (j.members.size() != 1 || !int64_of(*blob, n, 1, 0xffff)) return false;
    leaf.res = ResKind::Blob;
    leaf.blob = static_cast<int32_t>(n);
    return true;
  }
  const Json* rpk = j.get("rpk");
  if (!rpk) return false;
  if (rpk->type == Json::Null) return j.members.size() == 1;
  const Json* id = j.get("id");
  int64_t n = 0;
  if (rpk->type != Json::String || rpk->text.empty() || rpk->text.size() > 1024 ||
      !id || j.members.size() != 2 || !int64_of(*id, n, 0, 0xffff))
    return false;
  leaf.res = ResKind::Pack;
  leaf.pack = rpk->text;
  std::replace(leaf.pack.begin(), leaf.pack.end(), '/', '\\');
  leaf.local = static_cast<uint32_t>(n);
  return true;
}

bool string_of(const Json& j, Leaf& leaf) {
  leaf = Leaf{};
  leaf.type = LeafType::String;
  if (j.type == Json::Null) { leaf.null_string = true; return true; }
  // Same bound as File.write: readString then applies its own 1 MiB limit.
  if (j.type != Json::String || j.text.size() >= 0xfffffffeu) return false;
  leaf.text = j.text;
  return true;
}

// Self-typed value: integer, float (always with '.', 'e' or floatBits),
// string, null string, or resource object.
bool raw_of(const Json& j, Leaf& leaf) {
  leaf = Leaf{};
  switch (j.type) {
    case Json::Number:
      if (is_integer_token(j)) {
        int32_t v = 0;
        if (!int_of(j, v)) return false;
        leaf.word = static_cast<uint32_t>(v);
        return true;
      }
      leaf.type = LeafType::Float;
      return float_of(j, leaf.word);
    case Json::String:
    case Json::Null:
      return string_of(j, leaf);
    case Json::Object:
      if (j.get("floatBits")) { leaf.type = LeafType::Float; return float_of(j, leaf.word); }
      return resource_of(j, leaf);
    default:
      return false;
  }
}

// ---------------------------------------------------------------- encoder

// Reads the leaf stream and builds the named JSON tree.
class Enc {
 public:
  static constexpr bool encoding = true;
  Enc(const Document& doc, const std::vector<Leaf>& leaves) : doc_(doc), leaves_(leaves) {
    root.type = Json::Object;
    stack_.push_back(&root);
  }
  Json root;
  bool ok = true;

  bool at_end() const { return pos_ == leaves_.size(); }
  void fail() { ok = false; }

  int32_t i(const char* name) {
    const Leaf* leaf = next();
    if (!leaf) return 0;
    int32_t v = 0;
    if (leaf->type == LeafType::Int) v = static_cast<int32_t>(leaf->word);
    else if (!(leaf->type == LeafType::Resource && leaf->res == ResKind::Null)) { ok = false; return 0; }
    put(name, int_json(v));
    return v;
  }
  void f(const char* name) {
    const Leaf* leaf = next();
    if (!leaf || leaf->type != LeafType::Float) { ok = false; return; }
    put(name, float_json(leaf->word));
  }
  void s(const char* name) {
    const Leaf* leaf = next();
    if (!leaf || leaf->type != LeafType::String) { ok = false; return; }
    put(name, raw_json(*leaf));
  }
  bool r(const char* name) {
    const Leaf* leaf = next();
    if (!leaf) return false;
    Leaf value;
    value.type = LeafType::Resource;
    if (leaf->type == LeafType::Resource) value = *leaf;
    else if (!(leaf->type == LeafType::Int && leaf->word == 0)) { ok = false; return false; }
    put(name, resource_json(value, false));
    return value.res != ResKind::Null;
  }
  template <class F> void obj(const char* name, F body) { container(name, Json::Object, body); }
  template <class F> void arr(const char* name, F body) { container(name, Json::Array, body); }
  template <class F> void counted(const char* name, F item) {
    const Leaf* leaf = next();
    if (!leaf || leaf->type != LeafType::Int || static_cast<int32_t>(leaf->word) < 0 ||
        leaf->word > 1000000u) { ok = false; return; }
    const uint32_t n = leaf->word;
    arr(name, [&] { for (uint32_t k = 0; k < n && ok; ++k) item(); });
  }
  template <class P, class F>
  void until(const char* name, P is_end, uint32_t terminator, F item) {
    arr(name, [&] {
      while (ok) {
        const Leaf* leaf = peek();
        if (!leaf) { ok = false; return; }
        if (is_end(*leaf)) break;
        const size_t before = pos_;
        item();
        if (pos_ == before) ok = false;
      }
    });
    const Leaf* end = next();
    if (!end || !(end->type == LeafType::Int || end->type == LeafType::Resource) ||
        (end->type == LeafType::Int ? end->word != terminator
                                    : (terminator != 0 || end->res != ResKind::Null)))
      ok = false;
  }
  template <class P, class F> void repeat(const char* name, P more, F item) {
    arr(name, [&] {
      while (ok && more()) {
        const size_t before = pos_;
        item();
        if (pos_ == before) ok = false;
      }
    });
  }
  // Array element: an int flag, 0 for null, 1 followed by the item.
  template <class F> void flagged(F item) {
    const Leaf* leaf = next();
    if (!leaf || leaf->type != LeafType::Int || leaf->word > 1) { ok = false; return; }
    if (leaf->word == 0) put(nullptr, Json{});
    else item();
  }
  // An int selecting which key holds the item.
  int tag(const char* const* names, int count) {
    (void)names;
    const Leaf* leaf = next();
    if (!leaf || leaf->type != LeafType::Int || static_cast<int32_t>(leaf->word) < 0 ||
        static_cast<int32_t>(leaf->word) >= count) { ok = false; return -1; }
    return static_cast<int>(leaf->word);
  }
  void ints(const char* name, int n) { arr(name, [&] { for (int k = 0; k < n; ++k) i(nullptr); }); }
  void floats(const char* name, int n) { arr(name, [&] { for (int k = 0; k < n; ++k) f(nullptr); }); }

  bool racer_ahead() const {
    const Leaf* a = peek(0); const Leaf* b = peek(1);
    const Leaf* c = peek(2); const Leaf* d = peek(3);
    if (!a || !b || !c || !d || a->type != LeafType::Int || b->type != LeafType::Int ||
        c->type != LeafType::Float || d->type != LeafType::Int)
      return false;
    if (doc_.frames.empty()) return true;
    return d->frame >= 0 &&
        simple_name(doc_.frames[static_cast<size_t>(d->frame)].owner) == "VehicleDescriptor";
  }

  // The part object written by Part.save(File) (or read by Part.load):
  // the frame of its texture value names the part instance.
  const void* part_begin() {
    const Leaf* type = peek(0);
    const Leaf* texture = peek(2);
    if (!type || type->type != LeafType::Resource || type->res == ResKind::Null ||
        !texture || texture->frame < 0)
      return nullptr;
    const Frame& frame = doc_.frames[static_cast<size_t>(texture->frame)];
    if (!frame.self) return nullptr;
    if (!frame.self_class.empty()) put("class", Json::string(simple_name(frame.self_class)));
    return frame.self;
  }
  // Values the part's subclasses wrote after Part.save, grouped by script.
  template <class Spec> void part_data(const void* self, Spec spec) {
    if (!self || !ok) return;
    std::vector<std::pair<std::string, std::vector<Leaf>>> groups;
    while (const Leaf* leaf = peek()) {
      const Frame* owner = nullptr;
      for (int f = leaf->frame; f >= 0; f = doc_.frames[static_cast<size_t>(f)].parent)
        if (doc_.frames[static_cast<size_t>(f)].self == self) {
          owner = &doc_.frames[static_cast<size_t>(f)];
          break;
        }
      if (!owner) break;
      const std::string script = simple_name(owner->owner);
      if (groups.empty() || groups.back().first != script) groups.push_back({script, {}});
      groups.back().second.push_back(*leaf);
      ++pos_;
    }
    if (groups.empty()) return;
    arr("data", [&] {
      for (const auto& [script, leaves] : groups) {
        Json block = Json::of(Json::Object);
        block.add("script", Json::string(script));
        Enc sub(doc_, leaves);
        if (spec(sub, script) && sub.ok && sub.at_end()) {
          for (auto& member : sub.root.members) block.members.push_back(std::move(member));
        } else {
          Json values = Json::of(Json::Array);
          for (const Leaf& leaf : leaves) values.items.push_back(raw_json(leaf));
          block.add("values", std::move(values));
        }
        put(nullptr, std::move(block));
      }
    });
  }

 private:
  const Document& doc_;
  const std::vector<Leaf>& leaves_;
  size_t pos_ = 0;
  std::vector<Json*> stack_;

  const Leaf* peek(size_t k = 0) const {
    return pos_ + k < leaves_.size() ? &leaves_[pos_ + k] : nullptr;
  }
  const Leaf* next() {
    if (!ok || pos_ >= leaves_.size()) { ok = false; return nullptr; }
    return &leaves_[pos_++];
  }
  Json* put(const char* name, Json value) {
    Json* top = stack_.back();
    if (top->type == Json::Array) {
      top->items.push_back(std::move(value));
      return &top->items.back();
    }
    top->add(name ? name : "", std::move(value));
    return &top->members.back().second;
  }
  template <class F> void container(const char* name, Json::Type type, F body) {
    if (!ok) return;
    stack_.push_back(put(name, Json::of(type)));
    body();
    stack_.pop_back();
  }
};

// ---------------------------------------------------------------- decoder

// Walks the named JSON tree and rebuilds the leaf stream.
class Dec {
 public:
  static constexpr bool encoding = false;
  Dec(std::vector<Leaf>& out, const Json& root) : out_(out) { stack_.push_back({&root, 0}); }
  bool ok = true;

  void fail() { ok = false; }

  int32_t i(const char* name) {
    const Json* j = get(name);
    int32_t v = 0;
    if (!j || !int_of(*j, v)) { ok = false; return 0; }
    Leaf leaf;
    leaf.word = static_cast<uint32_t>(v);
    out_.push_back(std::move(leaf));
    return v;
  }
  void f(const char* name) {
    const Json* j = get(name);
    Leaf leaf;
    leaf.type = LeafType::Float;
    if (!j || !float_of(*j, leaf.word)) { ok = false; return; }
    out_.push_back(std::move(leaf));
  }
  void s(const char* name) {
    const Json* j = get(name);
    Leaf leaf;
    if (!j || !string_of(*j, leaf)) { ok = false; return; }
    out_.push_back(std::move(leaf));
  }
  bool r(const char* name) {
    const Json* j = get(name);
    Leaf leaf;
    if (!j || !resource_of(*j, leaf)) { ok = false; return false; }
    const bool present = leaf.res != ResKind::Null;
    out_.push_back(std::move(leaf));
    return present;
  }
  template <class F> void obj(const char* name, F body) { container(name, Json::Object, body); }
  template <class F> void arr(const char* name, F body) { container(name, Json::Array, body); }
  template <class F> void counted(const char* name, F item) {
    const Json* j = get(name);
    if (!j || j->type != Json::Array || j->items.size() > 1000000u) { ok = false; return; }
    Leaf count;
    count.word = static_cast<uint32_t>(j->items.size());
    out_.push_back(std::move(count));
    enter(j, [&] { for (size_t k = 0; k < j->items.size() && ok; ++k) item(); });
  }
  template <class P, class F>
  void until(const char* name, P is_end, uint32_t terminator, F item) {
    const Json* j = get(name);
    if (!j || j->type != Json::Array) { ok = false; return; }
    enter(j, [&] {
      for (size_t k = 0; k < j->items.size() && ok; ++k) {
        const size_t before = out_.size();
        item();
        if (ok && (out_.size() == before || is_end(out_[before]))) ok = false;
      }
    });
    Leaf end;
    end.word = terminator;
    out_.push_back(std::move(end));
  }
  template <class P, class F> void repeat(const char* name, P, F item) {
    const Json* j = get(name);
    if (!j || j->type != Json::Array) { ok = false; return; }
    enter(j, [&] { for (size_t k = 0; k < j->items.size() && ok; ++k) item(); });
  }
  template <class F> void flagged(F item) {
    Ctx& c = stack_.back();
    if (!ok || c.node->type != Json::Array || c.index >= c.node->items.size()) { ok = false; return; }
    Leaf flag;
    if (c.node->items[c.index].type == Json::Null) {
      ++c.index;
      out_.push_back(std::move(flag));
      return;
    }
    flag.word = 1;
    out_.push_back(std::move(flag));
    item();
  }
  int tag(const char* const* names, int count) {
    Ctx& c = stack_.back();
    int found = -1;
    for (int k = 0; k < count; ++k)
      if (c.node->get(names[k])) {
        if (found >= 0) { ok = false; return -1; }
        found = k;
      }
    if (found < 0) { ok = false; return -1; }
    Leaf leaf;
    leaf.word = static_cast<uint32_t>(found);
    out_.push_back(std::move(leaf));
    return found;
  }
  void ints(const char* name, int n) { arr(name, [&] { for (int k = 0; k < n; ++k) i(nullptr); }); }
  void floats(const char* name, int n) { arr(name, [&] { for (int k = 0; k < n; ++k) f(nullptr); }); }
  bool racer_ahead() const { return true; }
  const void* part_begin() { return this; }
  template <class Spec> void part_data(const void*, Spec spec) {
    if (!ok) return;
    const Json* data = stack_.back().node->get("data");
    if (!data) return;
    if (data->type != Json::Array) { ok = false; return; }
    for (const Json& block : data->items) {
      const Json* script = block.get("script");
      if (!script || script->type != Json::String) { ok = false; return; }
      if (const Json* values = block.get("values")) {
        if (values->type != Json::Array) { ok = false; return; }
        for (const Json& value : values->items) {
          Leaf leaf;
          if (!raw_of(value, leaf)) { ok = false; return; }
          out_.push_back(std::move(leaf));
        }
        continue;
      }
      Dec sub(out_, block);
      if (!spec(sub, script->text) || !sub.ok) { ok = false; return; }
    }
  }

 private:
  struct Ctx {
    const Json* node;
    size_t index;
  };
  std::vector<Leaf>& out_;
  std::vector<Ctx> stack_;

  const Json* get(const char* name) {
    if (!ok) return nullptr;
    Ctx& c = stack_.back();
    const Json* v = nullptr;
    if (c.node->type == Json::Array) {
      if (c.index < c.node->items.size()) v = &c.node->items[c.index++];
    } else if (name) {
      v = c.node->get(name);
    }
    if (!v) ok = false;
    return v;
  }
  template <class F> void enter(const Json* j, F body) {
    stack_.push_back({j, 0});
    body();
    if (j->type == Json::Array && stack_.back().index != j->items.size()) ok = false;
    stack_.pop_back();
  }
  template <class F> void container(const char* name, Json::Type type, F body) {
    const Json* j = get(name);
    if (!j || j->type != type) { ok = false; return; }
    enter(j, body);
  }
};

// ---------------------------------------------------------------- schema
// Each function mirrors one stock script's save()/load() order. The same
// code drives the encoder (stream -> names) and the decoder (names -> stream).

// Values a stock part subclass writes after super.save(File).
template <class IO> bool part_spec(IO& io, std::string_view script) {
  auto versioned = [&](auto fields) {
    const int32_t version = io.i("saveVersion");
    fields(version);
    return true;
  };
  if (script == "Chassis")
    return versioned([&](int32_t v) {
      if (v >= 1) {
        const int32_t wheels = io.i("wheels");
        if (wheels < 0 || wheels > 64) { io.fail(); return; }
        io.arr("wheelDamage", [&] { for (int32_t k = 0; k < wheels; ++k) io.s(nullptr); });
      }
      if (v >= 2) io.f("brakeBalance");
      if (v >= 3) io.f("mileage");
    });
  if (script == "Block")
    return versioned([&](int32_t v) {
      if (v >= 1) io.f("idleRpm");
      if (v >= 2) io.f("rpmLimit");
      if (v >= 3) {
        io.f("sparkTimeMin"); io.f("sparkTimeIncrease");
        io.f("sparkTimeRpm0"); io.f("sparkTimeRpm1");
      }
    });
  if (script == "Canister")
    return versioned([&](int32_t v) { if (v >= 1) io.f("amount"); });
  if (script == "Camshaft")
    return versioned([&](int32_t v) { if (v >= 1) io.f("advance"); });
  if (script == "Transmission")
    return versioned([&](int32_t v) {
      if (v >= 1) { io.floats("gearRatios", 8); io.f("finalDrive"); }
      if (v >= 2) { io.f("diffLock"); io.f("frontDrive"); }
    });
  if (script == "NOSInjectorSystem")
    return versioned([&](int32_t v) { if (v >= 1) io.f("nitroConsumption"); });
  if (script == "FuelInjectorSystem")
    return versioned([&](int32_t v) { if (v >= 1) { io.f("mixtureRatio"); io.i("fuelType"); } });
  if (script == "TurboCharger")
    return versioned([&](int32_t v) { if (v >= 1) io.f("wastegatePressure"); });
  if (script == "Tyre")
    return versioned([&](int32_t v) { if (v >= 1) io.f("inflation"); });
  if (script == "ShockAbsorber")
    return versioned([&](int32_t v) { if (v >= 1) { io.f("damping"); io.f("reboundFactor"); } });
  if (script == "Pneumatic_ShockAbsorber") {
    io.f("damping"); io.i("corner"); io.f("upLength"); io.f("downLength");
    return true;
  }
  return false;
}

// Part.save: header, body, slot tree, then subclass data.
template <class IO> void part(IO& io) {
  const void* self = io.part_begin();
  if (!io.r("type")) return;  // createFromFile reads nothing more
  io.i("partVersion");
  io.r("texture");
  io.r("mesh");
  io.f("wear");
  io.f("tear");
  io.until("slots",
           [](const Leaf& l) { return l.type == LeafType::Int && static_cast<int32_t>(l.word) == -1; },
           0xffffffffu, [&] {
             io.obj(nullptr, [&] {
               io.i("slot");
               io.s("damage");
               if (io.i("attachedSlot")) io.obj("part", [&] { part(io); });
             });
           });
  io.part_data(self, [](auto& sub, std::string_view script) { return part_spec(sub, script); });
}

template <class IO> void vehicle_descriptor(IO& io) {
  const int32_t version = io.i("saveVersion");
  if (version >= 1) {
    io.r("vehicle"); io.i("colorIndex"); io.f("power"); io.f("optical");
    io.f("tear"); io.f("wear"); io.f("stockPrestige"); io.f("fullPrestige");
  }
  if (version >= 2) io.s("name");
  if (version >= 3) { io.f("stockQM"); io.f("fullQM"); }
}

template <class IO> void inventory(IO& io, const char* name) {
  io.counted(name, [&] {
    io.obj(nullptr, [&] {
      static const char* const kinds[] = {"part", "folder"};
      const int kind = io.tag(kinds, 2);
      if (kind == 0) {
        io.obj("part", [&] { part(io); });
      } else if (kind == 1) {
        io.obj("folder", [&] {
          io.s("name");
          io.s("description");
          inventory(io, "items");
        });
      }
    });
  });
}

// GameLogic.save / GameLogic.load (save/carrer/<profile>/main).
template <class IO> void career(IO& io) {
  io.i("fileId");
  const int32_t version = io.i("fileVersion");
  if (version < 20) { io.fail(); return; }
  io.obj("player", [&] {
    io.r("character"); io.i("money"); io.f("prestige"); io.i("globalRank");
    io.i("flags"); io.s("name"); io.i("racesWon"); io.i("racesLost"); io.i("hints");
    if (version > 20) io.i("winPinkSlips");
  });
  inventory(io, "inventory");
  io.counted("decals", [&] { io.obj(nullptr, [&] { io.r("texture"); io.i("stickies"); }); });
  io.repeat("racers", [&] { return io.racer_ahead(); }, [&] {
    io.obj(nullptr, [&] {
      io.i("fileId"); io.i("seed"); io.f("prestige");
      io.obj("dayCar", [&] { vehicle_descriptor(io); });
      io.obj("nightCar", [&] { vehicle_descriptor(io); });
      io.i("racerSaveVersion");
      if (io.i("botSaveVersion") >= 1) { io.i("nightWins"); io.i("nightLosses"); io.f("bestNightQM"); }
    });
  });
  io.obj("carMarket", [&] {
    for (const char* list : {"new", "used"})
      io.counted(list, [&] { io.flagged([&] { io.obj(nullptr, [&] { vehicle_descriptor(io); }); }); });
  });
  io.i("currentCar");
  io.f("time");
  io.i("day");
  io.ints("reserved", 7);
}

// Vehicle.save / Vehicle.load (PlayerCarN).
template <class IO> void vehicle(IO& io) {
  io.i("fileId");
  if (io.i("fileVersion") != 5) { io.fail(); return; }
  io.obj("chassis", [&] { part(io); });
  for (const char* name : {"maxTestTrackSpeedSq", "bestTestTrackAcc", "bestTestTrackAcc120",
                           "bestTestTrackTime0", "bestTestTrackTime0SpeedSq", "bestTestTrackTime1",
                           "bestTestTrackTime2", "bestTestTrackTime2SpeedSq"})
    io.f(name);
  io.i("racesWon");
  io.i("racesLost");
  io.f("bestNightQM");
}

// Vehicle.saveSkin / Vehicle.loadSkin.
template <class IO> void skin(IO& io) {
  io.i("fileId");
  io.i("fileVersion");
  io.until("parts",
           [](const Leaf& l) { return !(l.type == LeafType::Resource && l.res != ResKind::Null); },
           0, [&] { io.obj(nullptr, [&] { io.r("type"); io.r("texture"); }); });
}

template <class IO> bool body_for(IO& io, std::string_view kind) {
  if (kind == "career") io.obj("career", [&] { career(io); });
  else if (kind == "vehicle") io.obj("vehicle", [&] { vehicle(io); });
  else if (kind == "skin") io.obj("skin", [&] { skin(io); });
  else return false;
  return io.ok;
}

Json header(const char* kind) {
  Json root = Json::of(Json::Object);
  root.add("format", Json::string("OpenSLRR-save"));
  root.add("version", int_json(2));
  root.add("kind", Json::string(kind));
  return root;
}

Json blobs_json(const Document& doc) {
  Json list = Json::of(Json::Array);
  for (const Blob& blob : doc.blobs) {
    Json o = Json::of(Json::Object);
    o.add("id", int_json(blob.ordinal));
    o.add("kind", int_json(blob.kind));
    o.add("base64", Json::string(base64_encode(blob.bytes)));
    list.items.push_back(std::move(o));
  }
  return list;
}

std::string text_of(const Json& root) {
  std::string out;
  write_json(root, out, 0);
  out += '\n';
  return out;
}

void append_word(std::string& out, uint32_t w) {
  for (int k = 0; k < 4; ++k) out += static_cast<char>((w >> (8 * k)) & 0xff);
}

std::string canonical(const Document& doc) {
  std::string out;
  for (const Leaf& leaf : doc.leaves) {
    switch (leaf.type) {
      case LeafType::Int:
      case LeafType::Float:
        out += 'w'; append_word(out, leaf.word); break;
      case LeafType::String:
        out += 'w';
        if (leaf.null_string) { append_word(out, 0); break; }
        append_word(out, static_cast<uint32_t>(leaf.text.size() + 1));
        out += 's'; append_word(out, static_cast<uint32_t>(leaf.text.size()));
        out += leaf.text;
        break;
      case LeafType::Resource:
        if (leaf.res == ResKind::Null) { out += 'w'; append_word(out, 0); break; }
        if (leaf.res == ResKind::Blob) { out += 'b'; append_word(out, static_cast<uint32_t>(leaf.blob)); break; }
        out += 'p';
        for (char c : leaf.pack) {
          if (c == '/') c = '\\';
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
          out += c;
        }
        out += '\0';
        append_word(out, leaf.local);
        break;
    }
  }
  std::vector<const Blob*> blobs;
  for (const Blob& blob : doc.blobs) blobs.push_back(&blob);
  std::sort(blobs.begin(), blobs.end(), [](const Blob* a, const Blob* b) { return a->ordinal < b->ordinal; });
  for (const Blob* blob : blobs) {
    out += 'B';
    append_word(out, static_cast<uint32_t>(blob->ordinal));
    append_word(out, static_cast<uint32_t>(blob->kind));
    append_word(out, static_cast<uint32_t>(blob->bytes.size()));
    out.append(blob->bytes.begin(), blob->bytes.end());
  }
  return out;
}

}  // namespace

std::string json_string(std::string_view bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : bytes) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 32 || c >= 127) {
      out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
    } else out += static_cast<char>(c);
  }
  return out + '"';
}

bool same_stream(const Document& a, const Document& b) { return canonical(a) == canonical(b); }

std::string encode(const Document& doc, std::string* kind) {
  const char* name = nullptr;
  if (!doc.leaves.empty() && doc.leaves[0].type == LeafType::Int) {
    const int32_t id = static_cast<int32_t>(doc.leaves[0].word);
    name = id == kCareerId ? "career" : id == kVehicleId ? "vehicle" : id == kSkinId ? "skin" : nullptr;
  }
  if (name) {
    Enc enc(doc, doc.leaves);
    if (body_for(enc, name) && enc.at_end() && enc.root.members.size() == 1) {
      Json root = header(name);
      root.add(name, std::move(enc.root.members[0].second));
      root.add("blobs", blobs_json(doc));
      std::string text = text_of(root);
      // Publish the named form only when it decodes to the same stream.
      Document back;
      if (decode(text, back) == 1 && same_stream(doc, back)) {
        if (kind) *kind = name;
        return text;
      }
    }
  }
  Json root = header("raw");
  Json values = Json::of(Json::Array);
  for (const Leaf& leaf : doc.leaves) values.items.push_back(raw_json(leaf));
  root.add("values", std::move(values));
  root.add("blobs", blobs_json(doc));
  if (kind) *kind = "raw";
  return text_of(root);
}

int decode(std::string_view text, Document& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    out = Document{};
    return -1;
  };
  Json root;
  Parser parser(text);
  if (!parser.parse(root) || root.type != Json::Object) return 0;
  const Json* format = root.get("format");
  const Json* version = root.get("version");
  if (!format || format->type != Json::String || format->text != "OpenSLRR-save" || !version)
    return 0;
  int32_t v = 0;
  if (int_of(*version, v) && v == 1) return 0;  // version 1 has its own reader
  if (v != 2) return fail("unsupported version");
  out = Document{};
  const Json* blobs = root.get("blobs");
  if (!blobs || blobs->type != Json::Array) return fail("blobs");
  for (const Json& item : blobs->items) {
    const Json* id = item.get("id");
    const Json* kind = item.get("kind");
    const Json* data = item.get("base64");
    int64_t ordinal = 0, blob_kind = 0;
    Blob blob;
    if (!id || !kind || !data || data->type != Json::String ||
        !int64_of(*id, ordinal, 1, 0xffff) || !int64_of(*kind, blob_kind, 0, 0x7fffffff) ||
        !base64_decode(data->text, blob.bytes))
      return fail("blob entry");
    blob.ordinal = static_cast<int32_t>(ordinal);
    blob.kind = static_cast<int32_t>(blob_kind);
    for (const Blob& other : out.blobs)
      if (other.ordinal == blob.ordinal) return fail("duplicate blob");
    out.blobs.push_back(std::move(blob));
  }
  const Json* kind = root.get("kind");
  if (!kind || kind->type != Json::String) return fail("kind");
  if (kind->text == "raw") {
    const Json* values = root.get("values");
    if (!values || values->type != Json::Array) return fail("values");
    for (const Json& value : values->items) {
      Leaf leaf;
      if (!raw_of(value, leaf)) return fail("value");
      out.leaves.push_back(std::move(leaf));
    }
  } else {
    Dec dec(out.leaves, root);
    if (!body_for(dec, kind->text)) return fail("named layout");
  }
  for (const Leaf& leaf : out.leaves) {
    if (leaf.type != LeafType::Resource || leaf.res != ResKind::Blob) continue;
    bool found = false;
    for (const Blob& blob : out.blobs) found = found || blob.ordinal == leaf.blob;
    if (!found) return fail("missing blob");
  }
  return 1;
}

}  // namespace slrr::save_json
