#include "test_fuzz_support.hpp"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>

namespace slrr::fuzz {
namespace {

constexpr size_t kDefaultIterations = 384;
constexpr size_t kMaxIterations = 2048;
constexpr size_t kMaxInputBytes = 512;

class Prng {
 public:
  explicit Prng(uint32_t seed) : state_(seed ? seed : 0x6d2b79f5u) {}

  uint32_t next() {
    uint32_t x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state_ = x;
    return x;
  }

  size_t below(size_t n) { return n ? next() % n : 0; }

 private:
  uint32_t state_;
};

struct Options {
  size_t iterations = kDefaultIterations;
  uint32_t seed = 0;
  std::filesystem::path repro_dir;
};

bool parse_u32(const std::string& text, uint32_t* value) {
  if (!value || text.empty()) return false;
  int base = 10;
  const char* first = text.data();
  const char* last = first + text.size();
  if (text.size() > 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    first += 2;
  }
  if (first == last) return false;
  unsigned long parsed = 0;
  const auto result = std::from_chars(first, last, parsed, base);
  if (result.ec != std::errc{} || result.ptr != last ||
      parsed > std::numeric_limits<uint32_t>::max())
    return false;
  *value = static_cast<uint32_t>(parsed);
  return true;
}

Result parse_options(int argc, char** argv, const char* lane,
                     uint32_t default_seed, Options* options) {
  options->seed = default_seed;
  options->repro_dir = std::filesystem::current_path() / "fuzz-repro";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help") {
      std::cout << "usage: " << lane
                << " [--iterations 0..2048] [--seed N] [--repro-dir PATH]\n";
      return fail("help");
    }
    if (i + 1 >= argc) return fail("missing value for " + arg);
    const std::string value = argv[++i];
    if (arg == "--iterations") {
      uint32_t parsed = 0;
      if (!parse_u32(value, &parsed) || parsed > kMaxIterations)
        return fail("--iterations must be in 0..2048");
      options->iterations = parsed;
    } else if (arg == "--seed") {
      if (!parse_u32(value, &options->seed))
        return fail("--seed must be a uint32 decimal or 0x hexadecimal value");
    } else if (arg == "--repro-dir") {
      if (value.empty()) return fail("--repro-dir requires a nonempty path");
      options->repro_dir = value;
    } else {
      return fail("unknown argument: " + arg);
    }
  }
  return pass();
}

bool write_bytes(const std::filesystem::path& path, const Bytes& bytes,
                 std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot create reproducer " + path.string();
    return false;
  }
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  out.close();
  if (!out) {
    *error = "cannot write reproducer " + path.string();
    return false;
  }
  return true;
}

Bytes mutate(const Bytes& seed, uint32_t case_seed) {
  Prng rng(case_seed);
  Bytes bytes = seed;
  const size_t operations = 1 + rng.below(6);
  for (size_t operation = 0; operation < operations; ++operation) {
    switch (rng.below(6)) {
      case 0:
        if (!bytes.empty()) bytes[rng.below(bytes.size())] ^= 1u << rng.below(8);
        break;
      case 1:
        if (!bytes.empty())
          bytes[rng.below(bytes.size())] = static_cast<uint8_t>(rng.next());
        break;
      case 2:
        bytes.resize(rng.below(bytes.size() + 1));
        break;
      case 3: {
        const size_t addition = 1 + rng.below(8);
        const size_t size = bytes.size() + addition;
        if (size <= kMaxInputBytes) {
          const size_t at = rng.below(bytes.size() + 1);
          bytes.insert(bytes.begin() + static_cast<Bytes::difference_type>(at),
                       addition, static_cast<uint8_t>(rng.next()));
        }
        break;
      }
      case 4: {
        const size_t size = rng.below(kMaxInputBytes + 1);
        if (size < bytes.size()) bytes.resize(size);
        break;
      }
      default:
        if (!bytes.empty()) {
          const size_t at = rng.below(bytes.size());
          const size_t count = 1 + rng.below(16);
          if (at + count <= bytes.size()) {
            bytes.erase(bytes.begin() + static_cast<Bytes::difference_type>(at),
                        bytes.begin() +
                            static_cast<Bytes::difference_type>(at + count));
          }
        }
        break;
    }
  }
  return bytes;
}

std::string case_name(const char* lane, size_t case_index, uint32_t seed) {
  std::ostringstream name;
  name << "slrr-" << lane << "-case-" << std::setw(5) << std::setfill('0')
       << case_index << "-seed-" << std::hex << std::setw(8) << seed;
  return name.str();
}

Result run_one(const char* lane, size_t case_index, uint32_t seed,
               const Bytes& bytes, const Options& options,
               const std::filesystem::path& current, Target target) {
  std::string io_error;
  if (!write_bytes(current, bytes, &io_error)) return fail(io_error);

  Result result;
  try {
    result = target(bytes, case_index, current);
  } catch (const std::exception& e) {
    result = fail(std::string("uncaught exception: ") + e.what());
  } catch (...) {
    result = fail("uncaught non-standard exception");
  }

  std::error_code ec;
  if (result.success) {
    std::filesystem::remove(current, ec);
    if (ec) return fail("cannot remove successful current-case file: " + ec.message());
    return pass();
  }

  const std::string base = case_name(lane, case_index, seed);
  const std::filesystem::path repro = options.repro_dir / (base + ".bin");
  std::filesystem::copy_file(current, repro,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    return fail(result.error + "; cannot save " + repro.string() + ": " +
                ec.message() + "; current bytes remain at " + current.string());
  }
  std::ostringstream seed_text;
  seed_text << std::hex << seed;
  const std::string metadata = std::string("lane=") + lane + "\ncase=" +
                               std::to_string(case_index) + "\nseed=0x" +
                               seed_text.str() + "\nbytes=" +
                               std::to_string(bytes.size()) + "\nerror=" +
                               result.error + "\n";
  if (!write_bytes(options.repro_dir / (base + ".txt"),
                   Bytes(metadata.begin(), metadata.end()), &io_error)) {
    return fail(result.error + "; " + io_error);
  }
  std::filesystem::remove(current, ec);
  std::cerr << "FAIL lane=" << lane << " case=" << case_index << " seed=0x"
            << std::hex << seed << std::dec << " error=" << result.error
            << "\nreproducer=" << repro.string() << "\n";
  return fail("reproducer saved");
}

}  // namespace

Result pass() { return {}; }
Result fail(std::string error) { return {false, std::move(error)}; }

void append_u32(Bytes* bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes->push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
}

void set_u32(Bytes* bytes, size_t offset, uint32_t value) {
  if (offset > bytes->size() || bytes->size() - offset < 4)
    bytes->resize(offset + 4, 0);
  for (unsigned i = 0; i < 4; ++i)
    (*bytes)[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
}

uint32_t read_u32(const Bytes& bytes, size_t offset) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (offset + i < bytes.size())
      value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
  }
  return value;
}

bool is_allocation_error(const std::string& error) {
  return error.find("bad allocation") != std::string::npos ||
         error.find("bad_alloc") != std::string::npos ||
         error.find("length_error") != std::string::npos ||
         error.find("vector too long") != std::string::npos;
}

int run_main(int argc, char** argv, const char* lane,
             const Bytes& mutation_seed,
             const std::vector<Bytes>& directed_cases, uint32_t default_seed,
             Target target) {
  Options options;
  const Result parsed = parse_options(argc, argv, lane, default_seed, &options);
  if (!parsed.success) {
    if (parsed.error == "help") return 0;
    std::cerr << lane << ": " << parsed.error
              << "\nusage: " << lane
              << " [--iterations 0..2048] [--seed N] [--repro-dir PATH]\n";
    return 2;
  }
  if (!target || mutation_seed.size() > kMaxInputBytes) {
    std::cerr << lane << ": invalid target or oversized mutation seed\n";
    return 2;
  }
  for (const Bytes& bytes : directed_cases) {
    if (bytes.size() > kMaxInputBytes) {
      std::cerr << lane << ": directed input exceeds 512-byte ceiling\n";
      return 2;
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(options.repro_dir, ec);
  if (ec) {
    std::cerr << lane << ": cannot create reproducer directory "
              << options.repro_dir.string() << ": " << ec.message() << "\n";
    return 2;
  }
  options.repro_dir = std::filesystem::absolute(options.repro_dir);
  const std::filesystem::path current =
      options.repro_dir / (std::string("slrr-") + lane + "-current.bin");

  size_t case_index = 0;
  for (const Bytes& bytes : directed_cases) {
    const Result result =
        run_one(lane, case_index, default_seed, bytes, options, current, target);
    if (!result.success) return 1;
    ++case_index;
  }

  std::cout << lane << ": directed=" << directed_cases.size()
            << " mutations=" << options.iterations << " seed=0x" << std::hex
            << options.seed << std::dec << " input_limit=" << kMaxInputBytes
            << " repro_dir=" << options.repro_dir.string() << "\n";
  for (size_t i = 0; i < options.iterations; ++i, ++case_index) {
    const uint32_t case_seed = options.seed + static_cast<uint32_t>(i) * 0x9e3779b9u;
    const Bytes bytes = mutate(mutation_seed, case_seed);
    const Result result =
        run_one(lane, case_index, case_seed, bytes, options, current, target);
    if (!result.success) return 1;
  }
  std::cout << lane << ": PASS cases=" << case_index << "\n";
  return 0;
}

}  // namespace slrr::fuzz
