#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace slrr::fuzz {

using Bytes = std::vector<uint8_t>;

struct Result {
  bool success = true;
  std::string error;
};

Result pass();
Result fail(std::string error);

// current_file contains bytes and may be opened by file-based parsers. It is
// written before every target call and retained if the process aborts.
using Target = Result (*)(const Bytes& bytes, size_t case_index,
                          const std::filesystem::path& current_file);

// Runs directed_cases followed by a fixed number of deterministic mutations of
// mutation_seed. The runner uses only the C++ standard library.
int run_main(int argc, char** argv, const char* lane,
             const Bytes& mutation_seed,
             const std::vector<Bytes>& directed_cases, uint32_t default_seed,
             Target target);

void append_u32(Bytes* bytes, uint32_t value);
void set_u32(Bytes* bytes, size_t offset, uint32_t value);
uint32_t read_u32(const Bytes& bytes, size_t offset);
bool is_allocation_error(const std::string& error);

}  // namespace slrr::fuzz
