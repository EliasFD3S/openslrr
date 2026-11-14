#include "test_harness.hpp"

#include "slrr/ptx_image.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

void append_u32(Bytes* bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes->push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

Bytes ptx_header(uint32_t mip_count) {
  Bytes bytes;
  append_u32(&bytes, 0);
  append_u32(&bytes, mip_count);
  append_u32(&bytes, 64);
  append_u32(&bytes, 32);
  return bytes;
}

void append_mip_header(Bytes* bytes, uint32_t total, uint32_t rgb,
                       uint32_t alpha) {
  append_u32(bytes, total);
  append_u32(bytes, rgb);
  append_u32(bytes, alpha);
  append_u32(bytes, 0);
  append_u32(bytes, 0);  // f32 level 0.0
}

void append_bytes(Bytes* bytes, std::initializer_list<uint8_t> payload) {
  bytes->insert(bytes->end(), payload.begin(), payload.end());
}

void expect_parse_result(const Bytes& bytes, bool accepted) {
  static uint32_t sequence = 0;
  const auto stamp = std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("slrr-safety-" + std::to_string(stamp) + "-" +
       std::to_string(sequence++) + ".ptx");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    SLRR_REQUIRE(static_cast<bool>(out));
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    SLRR_REQUIRE(static_cast<bool>(out));
  }

  const slrr::DdsImage image = slrr::load_ptx_file(path.string());
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (accepted) {
    SLRR_REQUIRE(image.error != "open failed");
    SLRR_REQUIRE(image.error != "too small");
    SLRR_REQUIRE(image.error != "read failed");
    SLRR_REQUIRE(image.error != "ptx parse failed");
  } else {
    SLRR_REQUIRE(!image.ok);
    SLRR_REQUIRE(image.error == "ptx parse failed" ||
                 image.error == "too small");
  }
}

}  // namespace

SLRR_TEST(ptx_accepts_single_rgb_level_at_exact_file_boundary) {
  Bytes bytes = ptx_header(1);
  append_mip_header(&bytes, 4, 4, 0);
  append_bytes(&bytes, {0xff, 0xd8, 0xff, 0xd9});
  expect_parse_result(bytes, true);
}

SLRR_TEST(ptx_accepts_multiple_complete_levels_with_alpha) {
  Bytes bytes = ptx_header(2);
  append_mip_header(&bytes, 4, 4, 0);
  append_bytes(&bytes, {0xff, 0xd8, 0xff, 0xd9});
  append_mip_header(&bytes, 8, 4, 4);
  append_bytes(&bytes, {0xff, 0xd8, 0xff, 0xd9,
                        0xff, 0xd8, 0xff, 0xd9});
  expect_parse_result(bytes, true);
}

SLRR_TEST(ptx_rejects_wrapped_rgb_alpha_total) {
  Bytes bytes = ptx_header(1);
  append_mip_header(&bytes, 0x20, 0xfffffff0, 0x30);
  bytes.resize(bytes.size() + 0x20, 0);
  bytes[36] = 0xff;
  bytes[37] = 0xd8;
  expect_parse_result(bytes, false);
}

SLRR_TEST(ptx_rejects_zero_and_one_byte_rgb) {
  Bytes zero_rgb = ptx_header(1);
  append_mip_header(&zero_rgb, 4, 0, 4);
  append_bytes(&zero_rgb, {0xff, 0xd8, 0xff, 0xd9});
  expect_parse_result(zero_rgb, false);

  Bytes one_rgb = ptx_header(1);
  append_mip_header(&one_rgb, 4, 1, 3);
  append_bytes(&one_rgb, {0xff, 0xd8, 0xff, 0xd9});
  expect_parse_result(one_rgb, false);
}

SLRR_TEST(ptx_rejects_one_byte_alpha) {
  Bytes bytes = ptx_header(1);
  append_mip_header(&bytes, 4, 3, 1);
  append_bytes(&bytes, {0xff, 0xd8, 0xd9, 0xff});
  expect_parse_result(bytes, false);
}

SLRR_TEST(ptx_rejects_truncated_headers_and_payload) {
  expect_parse_result({}, false);
  expect_parse_result(Bytes(15, 0), false);
  expect_parse_result(ptx_header(1), false);

  Bytes partial_mip = ptx_header(1);
  append_u32(&partial_mip, 4);
  append_u32(&partial_mip, 4);
  expect_parse_result(partial_mip, false);

  Bytes short_payload = ptx_header(1);
  append_mip_header(&short_payload, 8, 4, 4);
  append_bytes(&short_payload, {0xff, 0xd8, 0xff, 0xd9});
  expect_parse_result(short_payload, false);
}

SLRR_TEST(ptx_rejects_truncated_second_mip) {
  Bytes bytes = ptx_header(2);
  append_mip_header(&bytes, 4, 4, 0);
  append_bytes(&bytes, {0xff, 0xd8, 0xff, 0xd9});
  append_mip_header(&bytes, 4, 4, 0);
  append_bytes(&bytes, {0xff, 0xd8});
  expect_parse_result(bytes, false);
}

SLRR_TEST(ptx_rejects_inconsistent_total_and_bad_markers) {
  Bytes wrong_total = ptx_header(1);
  append_mip_header(&wrong_total, 3, 4, 0);
  append_bytes(&wrong_total, {0xff, 0xd8, 0xff, 0xd9});
  expect_parse_result(wrong_total, false);

  Bytes bad_rgb = ptx_header(1);
  append_mip_header(&bad_rgb, 4, 4, 0);
  append_bytes(&bad_rgb, {0xff, 0x00, 0xff, 0xd9});
  expect_parse_result(bad_rgb, false);

  Bytes bad_alpha = ptx_header(1);
  append_mip_header(&bad_alpha, 8, 4, 4);
  append_bytes(&bad_alpha, {0xff, 0xd8, 0xff, 0xd9,
                            0xff, 0x00, 0xff, 0xd9});
  expect_parse_result(bad_alpha, false);
}
