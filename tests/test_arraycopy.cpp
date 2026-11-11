#include "test_harness.hpp"

#include "natives_internal.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>

namespace {

slrr::Object int_array(std::initializer_list<int32_t> values) {
  slrr::Object array;
  array.is_array = true;
  for (const int32_t value : values)
    array.slots.push_back(slrr::Value::make_int(value));
  array.fields["length"] =
      slrr::Value::make_int(static_cast<int32_t>(array.slots.size()));
  return array;
}

void require_ints(const slrr::Object& array,
                  std::initializer_list<int32_t> expected) {
  SLRR_REQUIRE(array.slots.size() == expected.size());
  size_t i = 0;
  for (const int32_t value : expected) {
    SLRR_REQUIRE(array.slots[i].tag == slrr::ValueTag::Int);
    SLRR_REQUIRE(array.slots[i].v.i == value);
    ++i;
  }
}

void require_script_length(const slrr::Object& array, int32_t expected) {
  const auto length = array.fields.find("length");
  SLRR_REQUIRE(length != array.fields.end());
  SLRR_REQUIRE(length->second.tag == slrr::ValueTag::Int);
  SLRR_REQUIRE(length->second.v.i == expected);
}

}  // namespace

SLRR_TEST(arraycopy_valid_copy_exact_end_and_autogrow) {
  auto src = int_array({10, 20, 30, 40});
  auto dst = int_array({1, 2});

  slrr::native_arraycopy(&src, 1, &dst, 2, 3);

  require_ints(src, {10, 20, 30, 40});
  require_ints(dst, {1, 2, 20, 30, 40});
  require_script_length(dst, 5);
}

SLRR_TEST(arraycopy_exact_existing_destination_boundary) {
  auto src = int_array({7, 8});
  auto dst = int_array({1, 2, 3, 4});

  slrr::native_arraycopy(&src, 0, &dst, 2, 2);

  require_ints(dst, {1, 2, 7, 8});
  require_script_length(dst, 4);
}

SLRR_TEST(arraycopy_grow_fills_gap_and_supports_self_copy) {
  auto dst = int_array({9});
  auto src = int_array({4, 5});
  slrr::native_arraycopy(&src, 0, &dst, 3, 2);

  SLRR_REQUIRE(dst.slots.size() == 5);
  SLRR_REQUIRE(dst.slots[0].v.i == 9);
  for (size_t i = 1; i < 3; ++i) {
    SLRR_REQUIRE(dst.slots[i].tag == slrr::ValueTag::Obj);
    SLRR_REQUIRE(dst.slots[i].v.o == nullptr);
  }
  SLRR_REQUIRE(dst.slots[3].v.i == 4);
  SLRR_REQUIRE(dst.slots[4].v.i == 5);
  require_script_length(dst, 5);

  auto self = int_array({1, 2, 3});
  slrr::native_arraycopy(&self, 0, &self, 2, 3);
  require_ints(self, {1, 2, 1, 2, 3});
  require_script_length(self, 5);
}

SLRR_TEST(arraycopy_overlap_forward_and_backward) {
  auto forward = int_array({0, 1, 2, 3, 4, 5});
  slrr::native_arraycopy(&forward, 0, &forward, 2, 4);
  require_ints(forward, {0, 1, 0, 1, 2, 3});

  auto backward = int_array({0, 1, 2, 3, 4, 5});
  slrr::native_arraycopy(&backward, 2, &backward, 0, 4);
  require_ints(backward, {2, 3, 4, 5, 4, 5});
}

SLRR_TEST(arraycopy_rejects_negative_and_out_of_source_ranges) {
  auto src = int_array({10, 20, 30});
  auto dst = int_array({1, 2, 3});

  slrr::native_arraycopy(&src, -1, &dst, 0, 1);
  slrr::native_arraycopy(&src, 0, &dst, -1, 1);
  slrr::native_arraycopy(&src, 0, &dst, 0, -1);
  slrr::native_arraycopy(&src, 3, &dst, 0, 1);
  slrr::native_arraycopy(&src, 1, &dst, 0, 3);
  slrr::native_arraycopy(&src, 0, &dst, 0, 4);

  require_ints(src, {10, 20, 30});
  require_ints(dst, {1, 2, 3});
  require_script_length(dst, 3);
}

SLRR_TEST(arraycopy_rejects_int_max_and_overflow_without_mutation) {
  constexpr int32_t int_max = std::numeric_limits<int32_t>::max();
  auto src = int_array({10, 20, 30});
  auto dst = int_array({1, 2, 3});

  slrr::native_arraycopy(&src, 0, &dst, int_max, 1);
  slrr::native_arraycopy(&src, 0, &dst, int_max - 1, 2);
  slrr::native_arraycopy(&src, 0, &dst, int_max, int_max);
  slrr::native_arraycopy(&src, int_max, &dst, 0, 1);
  slrr::native_arraycopy(&src, 0, &dst, 0, int_max);

  require_ints(dst, {1, 2, 3});
  require_script_length(dst, 3);
}

SLRR_TEST(arraycopy_zero_length_and_invalid_objects_are_noops) {
  auto src = int_array({10});
  auto dst = int_array({1});
  slrr::native_arraycopy(&src, 0, &dst, std::numeric_limits<int32_t>::max(), 0);
  slrr::native_arraycopy(nullptr, 0, &dst, 0, 1);
  slrr::native_arraycopy(&src, 0, nullptr, 0, 1);

  slrr::Object not_array;
  slrr::native_arraycopy(&not_array, 0, &dst, 0, 1);
  slrr::native_arraycopy(&src, 0, &not_array, 0, 1);

  require_ints(dst, {1});
  require_script_length(dst, 1);
  SLRR_REQUIRE(not_array.slots.empty());
}
