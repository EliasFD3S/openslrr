#include "slrr/tufa.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace slrr {

// Exact Build 940 table (SLRR-JVM-decompiler / compiler OP_FLAGS).
const uint16_t kTreeOpFlags[48] = {
    0x0000, 0x1001, 0x1002, 0x1003, 0x0004, 0x0005, 0x1006, 0x1007, 0x1008,
    0x4009, 0x200a, 0x100b, 0x000c, 0x100d, 0x100e, 0x100f, 0x0010, 0x1011,
    0x4012, 0x0013, 0x1014, 0x1015, 0x1016, 0x1017, 0x0018, 0x1019, 0x101a,
    0x101b, 0x401c, 0x001d, 0x101e, 0x101f, 0x0020, 0x0021, 0x0022, 0x0023,
    0x0024, 0x4025, 0x4026, 0x1027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c,
    0x002d, 0x402e, 0x0000,
};

namespace {

class Reader {
 public:
  Reader(const uint8_t* p, size_t n, std::string label)
      : p_(p), n_(n), label_(std::move(label)) {}

  size_t left() const { return n_ - off_; }
  size_t offset() const { return off_; }

  void need(size_t n) const {
    if (n > left())
      throw std::runtime_error(label_ + ": truncated at 0x" +
                               hex(off_));
  }

  uint8_t u8() {
    need(1);
    return p_[off_++];
  }
  uint16_t u16() {
    need(2);
    uint16_t v = static_cast<uint16_t>(p_[off_] | (p_[off_ + 1] << 8));
    off_ += 2;
    return v;
  }
  uint32_t u32() {
    need(4);
    uint32_t v = static_cast<uint32_t>(p_[off_]) |
                 (static_cast<uint32_t>(p_[off_ + 1]) << 8) |
                 (static_cast<uint32_t>(p_[off_ + 2]) << 16) |
                 (static_cast<uint32_t>(p_[off_ + 3]) << 24);
    off_ += 4;
    return v;
  }
  std::string tag() {
    need(4);
    std::string t(reinterpret_cast<const char*>(p_ + off_), 4);
    off_ += 4;
    return t;
  }
  std::vector<uint8_t> take(size_t n) {
    need(n);
    std::vector<uint8_t> out(p_ + off_, p_ + off_ + n);
    off_ += n;
    return out;
  }
  void done() const {
    if (left())
      throw std::runtime_error(label_ + ": " + std::to_string(left()) +
                               " trailing bytes");
  }

 private:
  static std::string hex(size_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%zx", v);
    return buf;
  }
  const uint8_t* p_;
  size_t n_;
  size_t off_ = 0;
  std::string label_;
};

std::vector<TufaConstant> parse_constants(const std::vector<uint8_t>& bytes) {
  Reader r(bytes.data(), bytes.size(), "CONS");
  const uint32_t n = r.u32();
  std::vector<TufaConstant> out;
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    TufaConstant c;
    const uint32_t kind = r.u32();
    switch (kind) {
      case 0: {
        c.kind = TufaConstKind::Utf8;
        const uint32_t len = r.u32();
        auto raw = r.take(len);
        c.text.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        if (r.u8() != 0) throw std::runtime_error("CONS: missing NUL");
        break;
      }
      case 3:
        c.kind = TufaConstKind::Resource;
        c.a = r.u32();
        c.b = r.u32();
        break;
      case 4:
        c.kind = TufaConstKind::Class;
        c.a = r.u32();
        break;
      case 5:
        c.kind = TufaConstKind::Reference;
        c.a = r.u32();
        c.b = r.u32();
        break;
      case 7:
        c.kind = TufaConstKind::NameAndType;
        c.a = r.u32();
        c.b = r.u32();
        break;
      default:
        throw std::runtime_error("CONS: unknown kind " + std::to_string(kind));
    }
    out.push_back(std::move(c));
  }
  r.done();
  return out;
}

void parse_members(const std::vector<uint8_t>* bytes, bool methods,
                   std::vector<TufaMember>* statics,
                   std::vector<TufaMember>* instances) {
  statics->clear();
  instances->clear();
  if (!bytes || bytes->empty()) return;
  Reader r(bytes->data(), bytes->size(), methods ? "MTHD" : "FILD");
  auto read_part = [&](std::vector<TufaMember>* dest) {
    const uint32_t n = r.u32();
    dest->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      TufaMember m;
      m.flags = r.u32();
      m.name = r.u32();
      m.descriptor = r.u32();
      m.tree = r.u32();
      if (methods) m.locals = r.u32();
      dest->push_back(m);
    }
  };
  read_part(statics);
  read_part(instances);
  r.done();
}

std::vector<std::vector<TufaNode>> parse_trees(const std::vector<uint8_t>& bytes) {
  Reader r(bytes.data(), bytes.size(), "TREE");
  const uint32_t n = r.u32();
  std::vector<std::vector<TufaNode>> trees;
  trees.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t len = r.u32();
    std::vector<TufaNode> nodes;
    nodes.reserve(len);
    for (uint32_t j = 0; j < len; ++j) {
      TufaNode node;
      node.op = r.u8();
      if (node.op >= 48 || node.op == 0 || node.op == 47)
        throw std::runtime_error("TREE: invalid opcode " +
                                 std::to_string(node.op));
      node.line = r.u16();
      node.wide = (kTreeOpFlags[node.op] >> 12) != 0;
      if (node.wide) node.imm = r.u32();
      nodes.push_back(node);
    }
    trees.push_back(std::move(nodes));
  }
  r.done();
  return trees;
}

TufaClass parse_one_blob(Reader& file) {
  if (file.tag() != "TUFA") throw std::runtime_error("Expected TUFA tag");
  if (file.u32() != 4) throw std::runtime_error("Expected TUFA header size 4");
  TufaClass cls;
  cls.build = file.u32();

  std::vector<uint8_t> cons, fild, mthd, clss, tree;
  bool have_fild = false, have_mthd = false;
  while (tree.empty()) {
    const std::string tag = file.tag();
    const uint32_t len = file.u32();
    auto data = file.take(len);
    if (tag == "CONS")
      cons = std::move(data);
    else if (tag == "FILD") {
      fild = std::move(data);
      have_fild = true;
    } else if (tag == "MTHD") {
      mthd = std::move(data);
      have_mthd = true;
    } else if (tag == "CLSS")
      clss = std::move(data);
    else if (tag == "TREE")
      tree = std::move(data);
    else
      throw std::runtime_error("Unexpected chunk " + tag);
  }
  if (cons.empty() || clss.empty() || tree.empty())
    throw std::runtime_error("TUFA blob missing CONS/CLSS/TREE");

  cls.constants = parse_constants(cons);
  parse_members(have_fild ? &fild : nullptr, false, &cls.static_fields,
                &cls.instance_fields);
  parse_members(have_mthd ? &mthd : nullptr, true, &cls.static_methods,
                &cls.instance_methods);

  Reader c(clss.data(), clss.size(), "CLSS");
  if (c.u32() != kTufaNone) throw std::runtime_error("Invalid CLSS marker");
  cls.is_interface = c.u32() != 0;
  cls.name_index = c.u32();
  cls.super_index = c.u32();
  const uint32_t ni = c.u32();
  cls.interfaces.reserve(ni);
  for (uint32_t i = 0; i < ni; ++i) cls.interfaces.push_back(c.u32());
  c.done();

  cls.trees = parse_trees(tree);

  // Touch resolved names (validate indices).
  (void)cls.name();
  if (cls.super_index != kTufaNone) (void)cls.super_name();
  for (uint32_t ix : cls.interfaces) (void)cls.class_name(ix);
  auto check_members = [&](const std::vector<TufaMember>& members) {
    for (const auto& mem : members) {
      (void)cls.utf8(mem.name);
      (void)cls.utf8(mem.descriptor);
      if (mem.tree != kTufaNone && mem.tree >= cls.trees.size())
        throw std::runtime_error(cls.name() + ": tree index OOB");
    }
  };
  check_members(cls.static_fields);
  check_members(cls.instance_fields);
  check_members(cls.static_methods);
  check_members(cls.instance_methods);
  return cls;
}

}  // namespace

const std::string& TufaClass::utf8(uint32_t index) const {
  if (index >= constants.size() ||
      constants[index].kind != TufaConstKind::Utf8)
    throw std::runtime_error("expected UTF8 constant");
  return constants[index].text;
}

std::string TufaClass::class_name(uint32_t index) const {
  if (index >= constants.size() ||
      constants[index].kind != TufaConstKind::Class)
    throw std::runtime_error("expected class constant");
  return utf8(constants[index].a);
}

std::string TufaClass::name() const { return class_name(name_index); }

std::string TufaClass::super_name() const {
  if (super_index == kTufaNone) return {};
  return class_name(super_index);
}

TufaClass::Ref TufaClass::reference(uint32_t index) const {
  if (index >= constants.size())
    throw std::runtime_error("expected reference constant");
  const auto& c = constants[index];
  // Soft: method/field ops usually CONS Reference; some trees point at
  // NameAndType (current class) or Utf8 (bare name) instead.
  if (c.kind == TufaConstKind::Reference) {
    if (c.b >= constants.size() ||
        constants[c.b].kind != TufaConstKind::NameAndType)
      throw std::runtime_error("expected nameAndType constant");
    const auto& nat = constants[c.b];
    return Ref{class_name(c.a), utf8(nat.a), utf8(nat.b)};
  }
  if (c.kind == TufaConstKind::NameAndType)
    return Ref{name(), utf8(c.a), utf8(c.b)};
  if (c.kind == TufaConstKind::Utf8) return Ref{name(), c.text, {}};
  throw std::runtime_error("expected reference constant");
}

TufaClass::Resource TufaClass::resource(uint32_t index) const {
  if (index >= constants.size() ||
      constants[index].kind != TufaConstKind::Resource)
    throw std::runtime_error("expected resource constant");
  const auto& r = constants[index];
  return Resource{utf8(r.a), r.b};
}

bool tufa_parse(const uint8_t* data, size_t size, std::vector<TufaClass>* out,
                std::string* err) {
  try {
    out->clear();
    Reader r(data, size, "TUFA");
    while (r.left()) out->push_back(parse_one_blob(r));
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

bool tufa_load_file(const std::string& path, std::vector<TufaClass>* out,
                    std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto n = static_cast<size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(n);
  if (n && !in.read(reinterpret_cast<char*>(buf.data()),
                    static_cast<std::streamsize>(n))) {
    if (err) *err = "read failed " + path;
    return false;
  }
  return tufa_parse(buf.data(), buf.size(), out, err);
}

}  // namespace slrr
