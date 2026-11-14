#include "test_fuzz_support.hpp"

#include "slrr/ptx_image.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using slrr::fuzz::Bytes;
using slrr::fuzz::Result;

Bytes header(uint32_t mip_count) {
  Bytes bytes;
  slrr::fuzz::append_u32(&bytes, 0);
  slrr::fuzz::append_u32(&bytes, mip_count);
  slrr::fuzz::append_u32(&bytes, 64);
  slrr::fuzz::append_u32(&bytes, 32);
  return bytes;
}

void mip_header(Bytes* bytes, uint32_t total, uint32_t rgb, uint32_t alpha) {
  slrr::fuzz::append_u32(bytes, total);
  slrr::fuzz::append_u32(bytes, rgb);
  slrr::fuzz::append_u32(bytes, alpha);
  slrr::fuzz::append_u32(bytes, 0);
  slrr::fuzz::append_u32(bytes, 0);
}

void jpeg4(Bytes* bytes) {
  bytes->insert(bytes->end(), {0xff, 0xd8, 0xff, 0xd9});
}

Bytes valid_single() {
  Bytes bytes = header(1);
  mip_header(&bytes, 4, 4, 0);
  jpeg4(&bytes);
  return bytes;
}

Bytes valid_alpha_pair() {
  Bytes bytes = header(2);
  mip_header(&bytes, 4, 4, 0);
  jpeg4(&bytes);
  mip_header(&bytes, 8, 4, 4);
  jpeg4(&bytes);
  jpeg4(&bytes);
  return bytes;
}

Bytes wrapped_total() {
  Bytes bytes = header(1);
  mip_header(&bytes, 0x20, 0xfffffff0u, 0x30);
  bytes.resize(bytes.size() + 0x20, 0);
  bytes[36] = 0xff;
  bytes[37] = 0xd8;
  return bytes;
}

Bytes one_byte_alpha() {
  Bytes bytes = header(1);
  mip_header(&bytes, 4, 3, 1);
  bytes.insert(bytes.end(), {0xff, 0xd8, 0xd9, 0xff});
  return bytes;
}

Bytes truncated_second_mip() {
  Bytes bytes = header(2);
  mip_header(&bytes, 4, 4, 0);
  jpeg4(&bytes);
  mip_header(&bytes, 4, 4, 0);
  bytes.insert(bytes.end(), {0xff, 0xd8});
  return bytes;
}

Bytes bad_alpha_marker() {
  Bytes bytes = header(1);
  mip_header(&bytes, 8, 4, 4);
  jpeg4(&bytes);
  bytes.insert(bytes.end(), {0xff, 0x00, 0xff, 0xd9});
  return bytes;
}

Result target(const Bytes&, size_t case_index,
              const std::filesystem::path& current_file) {
  const slrr::DdsImage image = slrr::load_ptx_file(current_file.string());
  if (case_index < 2) {
    if (image.ok || image.width != 64 || image.height != 32 ||
        image.error != "ptx needs SLRR_HAS_LIBBGFX (stb_image)")
      return slrr::fuzz::fail("valid PTX seed did not reach the no-stb boundary");
    return slrr::fuzz::pass();
  }
  if (case_index < 6) {
    if (image.ok || (image.error != "ptx parse failed" && image.error != "too small"))
      return slrr::fuzz::fail("malformed directed PTX input was not rejected");
  }
  return slrr::fuzz::pass();
}

}  // namespace

int main(int argc, char** argv) {
  const Bytes valid = valid_single();
  const std::vector<Bytes> directed = {
      valid, valid_alpha_pair(), wrapped_total(), one_byte_alpha(),
      truncated_second_mip(), bad_alpha_marker(),
  };
  return slrr::fuzz::run_main(argc, argv, "ptx", valid, directed, 0x70745831u,
                              &target);
}
