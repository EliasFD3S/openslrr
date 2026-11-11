#include "test_fuzz_support.hpp"

#include "slrr/rpk_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using slrr::fuzz::Bytes;
using slrr::fuzz::Result;

Bytes rpak(uint32_t dep_a, uint32_t dep_b, uint32_t table_size, uint32_t count) {
  Bytes bytes;
  bytes.insert(bytes.end(), {'R', 'P', 'A', 'K'});
  slrr::fuzz::append_u32(&bytes, 0x200);
  slrr::fuzz::append_u32(&bytes, dep_a);
  slrr::fuzz::append_u32(&bytes, dep_b);
  slrr::fuzz::append_u32(&bytes, table_size);
  slrr::fuzz::append_u32(&bytes, count);
  slrr::fuzz::append_u32(&bytes, 0);
  slrr::fuzz::append_u32(&bytes, 0);
  return bytes;
}

void valid_entry(Bytes* bytes, uint32_t type_id, uint32_t offset, uint32_t size) {
  slrr::fuzz::append_u32(bytes, 0);
  slrr::fuzz::append_u32(bytes, type_id);
  bytes->push_back(1);
  bytes->push_back(0);
  slrr::fuzz::append_u32(bytes, 0);
  slrr::fuzz::append_u32(bytes, offset);
  slrr::fuzz::append_u32(bytes, size);
  bytes->push_back(0);                     // alias length
}

Bytes valid_pack() {
  Bytes bytes = rpak(0, 0, 23, 1);
  valid_entry(&bytes, 0x1234, 0, 0);
  return bytes;
}

Bytes huge_count() {
  Bytes bytes = rpak(0, 0, 23, 0xffffffffu);
  valid_entry(&bytes, 0x1234, 0, 0);
  return bytes;
}

Bytes huge_payload() {
  Bytes bytes = rpak(0, 0, 23, 1);
  valid_entry(&bytes, 0x1234, 0, 0xffffffffu);
  return bytes;
}

Bytes overflowing_dependencies() {
  Bytes bytes = rpak(0xffffffffu, 1, 23, 1);
  valid_entry(&bytes, 0x1234, 0, 0);
  return bytes;
}

Result target(const Bytes&, size_t case_index,
              const std::filesystem::path& current_file) {
  slrr::RpkIndex index;
  index.set_root(current_file.parent_path().string());
  const int32_t id = index.open_lib(current_file.filename().string());
  const slrr::RpkPack* pack = index.pack_by_id(id);
  if (!pack) return slrr::fuzz::fail("open_lib did not retain its pack");

  if (case_index == 0) {
    if (pack->entries.size() != 1 || pack->entries[0].type_id != 0x1234)
      return slrr::fuzz::fail("valid RPK seed was not parsed");
  } else if (case_index == 1 || case_index == 3) {
    if (!pack->entries.empty())
      return slrr::fuzz::fail("impossible RPK count was not rejected before parsing");
  } else if (case_index == 2) {
    if (pack->entries.size() != 1)
      return slrr::fuzz::fail("payload reproducer did not retain its entry");
    const uint32_t full = slrr::RpkIndex::make_full_id(id, 0x1234);
    if (!index.read_payload(full).empty())
      return slrr::fuzz::fail("out-of-file RPK payload returned bytes");
  }

  // Exercise every accepted payload range. Production bounds must keep this
  // proportional to the current <=512-byte file, not a declared u32 size.
  for (const slrr::RpkEntry& item : pack->entries) {
    const uint32_t full = slrr::RpkIndex::make_full_id(id, item.type_id);
    (void)index.read_payload(full);
  }
  return slrr::fuzz::pass();
}

}  // namespace

int main(int argc, char** argv) {
  const Bytes valid = valid_pack();
  const std::vector<Bytes> directed = {
      valid, huge_count(), huge_payload(), overflowing_dependencies(),
  };
  return slrr::fuzz::run_main(argc, argv, "rpk", valid, directed, 0x52504b32u,
                              &target);
}
