#include "test_fuzz_support.hpp"

#include "natives_internal.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using slrr::fuzz::Bytes;
using slrr::fuzz::Result;

Bytes command(uint32_t source_size, uint32_t destination_size, int32_t source_pos,
              int32_t destination_pos, int32_t length) {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, source_size);
  slrr::fuzz::append_u32(&bytes, destination_size);
  slrr::fuzz::append_u32(&bytes, static_cast<uint32_t>(source_pos));
  slrr::fuzz::append_u32(&bytes, static_cast<uint32_t>(destination_pos));
  slrr::fuzz::append_u32(&bytes, static_cast<uint32_t>(length));
  return bytes;
}

slrr::Object make_array(size_t size, int32_t first) {
  slrr::Object array;
  array.is_array = true;
  for (size_t i = 0; i < size; ++i)
    array.slots.push_back(slrr::Value::make_int(first + static_cast<int32_t>(i)));
  array.fields["length"] = slrr::Value::make_int(static_cast<int32_t>(size));
  return array;
}

Result check_array(const slrr::Object& array, const std::vector<int32_t>& expected,
                   const char* which) {
  if (array.slots.size() != expected.size())
    return slrr::fuzz::fail(std::string(which) + ": slot count differs");
  for (size_t i = 0; i < expected.size(); ++i) {
    if (expected[i] == INT32_MIN) {
      if (array.slots[i].tag != slrr::ValueTag::Obj ||
          array.slots[i].v.o != nullptr)
        return slrr::fuzz::fail(std::string(which) + ": gap slot differs at " +
                                std::to_string(i));
    } else if (array.slots[i].tag != slrr::ValueTag::Int ||
               array.slots[i].v.i != expected[i]) {
      return slrr::fuzz::fail(std::string(which) + ": slot value differs at " +
                              std::to_string(i));
    }
  }
  const auto length = array.fields.find("length");
  if (length == array.fields.end() || length->second.tag != slrr::ValueTag::Int ||
      length->second.v.i != static_cast<int32_t>(expected.size()))
    return slrr::fuzz::fail(std::string(which) + ": script length differs");
  return slrr::fuzz::pass();
}

Result target(const Bytes& bytes, size_t case_index,
              const std::filesystem::path&) {
  const size_t source_size = slrr::fuzz::read_u32(bytes, 0) % 13;
  const size_t destination_size = slrr::fuzz::read_u32(bytes, 4) % 13;
  const int32_t source_pos = static_cast<int32_t>(slrr::fuzz::read_u32(bytes, 8));
  const int32_t destination_pos = static_cast<int32_t>(slrr::fuzz::read_u32(bytes, 12));
  const int32_t length = static_cast<int32_t>(slrr::fuzz::read_u32(bytes, 16));

  std::vector<int32_t> source_values;
  std::vector<int32_t> destination_values;
  for (size_t i = 0; i < source_size; ++i)
    source_values.push_back(100 + static_cast<int32_t>(i));
  for (size_t i = 0; i < destination_size; ++i)
    destination_values.push_back(200 + static_cast<int32_t>(i));
  std::vector<int32_t> expected_source = source_values;
  std::vector<int32_t> expected_destination =
      case_index % 2 == 0 ? destination_values : source_values;

  bool valid = length > 0 && source_pos >= 0 && destination_pos >= 0;
  size_t need = 0;
  if (valid) {
    const size_t sp = static_cast<size_t>(source_pos);
    const size_t dp = static_cast<size_t>(destination_pos);
    const size_t count = static_cast<size_t>(length);
    valid = sp <= source_values.size() && count <= source_values.size() - sp;
    if (valid) {
      constexpr size_t int32_max = static_cast<size_t>(INT32_MAX);
      valid = dp <= int32_max && count <= int32_max - dp;
      if (valid) need = dp + count;
    }
  }

  // This lane's work ceiling deliberately avoids multi-gigabyte auto-grow.
  // Out-of-source-range and other invalid extreme values still execute.
  if (valid && need > 64) return slrr::fuzz::pass();

  if (valid) {
    if (need > expected_destination.size())
      expected_destination.resize(need, INT32_MIN);
    const std::vector<int32_t> temporary(
        expected_source.begin() + source_pos,
        expected_source.begin() + source_pos + length);
    for (size_t i = 0; i < static_cast<size_t>(length); ++i)
      expected_destination[static_cast<size_t>(destination_pos) + i] = temporary[i];
  }

  auto source = make_array(source_size, 100);
  auto destination = make_array(destination_size, 200);
  if (case_index % 2 == 0) {
    slrr::native_arraycopy(&source, source_pos, &destination, destination_pos, length);
    Result result = check_array(source, expected_source, "source");
    if (!result.success) return result;
    return check_array(destination, expected_destination, "destination");
  }

  slrr::native_arraycopy(&source, source_pos, &source, destination_pos, length);
  return check_array(source, expected_destination, "self-copy");
}

}  // namespace

int main(int argc, char** argv) {
  const Bytes valid = command(6, 4, 0, 2, 4);
  const std::vector<Bytes> directed = {
      valid,
      command(6, 6, 0, 2, 4),
      command(6, 6, 2, 0, 4),
      command(3, 3, -1, 0, 1),
      command(3, 3, 0, 0, -1),
      command(3, 3, 3, 0, 1),
      command(3, 3, 0, INT32_MAX, 1),
      command(1, 1, 0, INT32_MAX - 1, 1),  // skipped by the 64-slot work ceiling
      command(2, 2, 0, 0, 0),
  };
  return slrr::fuzz::run_main(argc, argv, "arraycopy", valid, directed,
                              0xacc04321u, &target);
}
