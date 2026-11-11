#include "test_fuzz_support.hpp"

#include "slrr/tufa.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using slrr::fuzz::Bytes;
using slrr::fuzz::Result;

Bytes valid_constants() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 2);
  slrr::fuzz::append_u32(&bytes, 4);  // Class
  slrr::fuzz::append_u32(&bytes, 1);  // UTF8 index
  slrr::fuzz::append_u32(&bytes, 0);  // Utf8
  slrr::fuzz::append_u32(&bytes, 1);
  bytes.push_back('C');
  bytes.push_back(0);
  return bytes;
}

Bytes valid_class() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, slrr::kTufaNone);
  slrr::fuzz::append_u32(&bytes, 0);  // not an interface
  slrr::fuzz::append_u32(&bytes, 0);  // name Class constant
  slrr::fuzz::append_u32(&bytes, slrr::kTufaNone);
  slrr::fuzz::append_u32(&bytes, 0);  // interface count
  return bytes;
}

Bytes valid_trees() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 0);
  return bytes;
}

void chunk(Bytes* bytes, const char tag[5], const Bytes& data) {
  bytes->insert(bytes->end(), tag, tag + 4);
  slrr::fuzz::append_u32(bytes, static_cast<uint32_t>(data.size()));
  bytes->insert(bytes->end(), data.begin(), data.end());
}

Bytes blob(const Bytes& constants, const Bytes& class_data, const Bytes& trees,
           const Bytes* fields = nullptr, const Bytes* methods = nullptr) {
  Bytes bytes;
  bytes.insert(bytes.end(), {'T', 'U', 'F', 'A'});
  slrr::fuzz::append_u32(&bytes, 4);
  slrr::fuzz::append_u32(&bytes, 940);
  chunk(&bytes, "CONS", constants);
  if (fields) chunk(&bytes, "FILD", *fields);
  if (methods) chunk(&bytes, "MTHD", *methods);
  chunk(&bytes, "CLSS", class_data);
  chunk(&bytes, "TREE", trees);
  return bytes;
}

Bytes huge_constant_count() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 0xffffffffu);
  return bytes;
}

Bytes huge_member_count() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 0xffffffffu);
  return bytes;
}

Bytes huge_tree_count() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 0xffffffffu);
  return bytes;
}

Bytes huge_node_count() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 1);
  slrr::fuzz::append_u32(&bytes, 0xffffffffu);
  return bytes;
}

Bytes huge_interface_count() {
  Bytes bytes = valid_class();
  slrr::fuzz::set_u32(&bytes, 16, 0xffffffffu);
  return bytes;
}

Bytes huge_utf8_length() {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 1);
  slrr::fuzz::append_u32(&bytes, 0);
  slrr::fuzz::append_u32(&bytes, 0xffffffffu);
  return bytes;
}

Result target(const Bytes& bytes, size_t case_index,
              const std::filesystem::path&) {
  std::vector<slrr::TufaClass> classes;
  std::string error;
  const bool parsed = slrr::tufa_parse(bytes.empty() ? nullptr : bytes.data(),
                                       bytes.size(), &classes, &error);
  if (slrr::fuzz::is_allocation_error(error))
    return slrr::fuzz::fail("attacker-declared count caused allocation failure: " + error);

  if (case_index == 0) {
    if (!parsed || classes.size() != 1 || classes[0].name() != "C")
      return slrr::fuzz::fail("valid TUFA seed was not parsed: " + error);
    return slrr::fuzz::pass();
  }

  if (case_index >= 1 && case_index <= 7) {
    if (parsed) return slrr::fuzz::fail("malformed directed TUFA input was accepted");
    const char* expected[] = {"", "CONS", "MTHD", "FILD", "TREE",
                              "TREE", "CLSS", "CONS"};
    if (error.find(expected[case_index]) == std::string::npos)
      return slrr::fuzz::fail("TUFA rejection did not identify the bounded chunk: " + error);
  }
  return slrr::fuzz::pass();
}

}  // namespace

int main(int argc, char** argv) {
  const Bytes constants = valid_constants();
  const Bytes class_data = valid_class();
  const Bytes trees = valid_trees();
  const Bytes valid = blob(constants, class_data, trees);
  const Bytes huge_methods = huge_member_count();
  const Bytes huge_fields = huge_member_count();
  const std::vector<Bytes> directed = {
      valid,
      blob(huge_constant_count(), class_data, trees),
      blob(constants, class_data, trees, nullptr, &huge_methods),
      blob(constants, class_data, trees, &huge_fields, nullptr),
      blob(constants, class_data, huge_tree_count()),
      blob(constants, class_data, huge_node_count()),
      blob(constants, huge_interface_count(), trees),
      blob(huge_utf8_length(), class_data, trees),
  };
  return slrr::fuzz::run_main(argc, argv, "tufa", valid, directed, 0x54554641u,
                              &target);
}
