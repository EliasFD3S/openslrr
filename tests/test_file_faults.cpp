#include "test_harness.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/file_faults.hpp"
#include "slrr/metrics.hpp"
#include "support/test_fixtures.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace ff = slrr::file_faults;
namespace fs = std::filesystem;

struct Reset {
  Reset() {
    ff::reset();
    slrr::metrics::reset();
    slrr::diagnostics::shutdown();
  }
  ~Reset() {
    ff::reset();
    slrr::diagnostics::shutdown();
  }
};

ff::Rule rule(ff::Operation operation, ff::Fault fault,
              std::uint64_t after_operations = 0, fs::path path = {}) {
  ff::Rule value;
  value.operation = operation;
  value.fault = fault;
  value.fail_after_operations = after_operations;
  value.path = std::move(path);
  return value;
}

ff::Rule byte_rule(ff::Operation operation, ff::Fault fault,
                   std::uint64_t after_bytes, fs::path path = {}) {
  auto value = rule(operation, fault, ff::kUnlimited, std::move(path));
  value.fail_after_bytes = after_bytes;
  return value;
}

slrr::test::Bytes bytes(std::string_view text) {
  return slrr::test::Bytes(text.begin(), text.end());
}

// Test-only stream adapter. It exercises real temporary files with the same
// interception API used by natives_io.cpp; it is not a production persistence
// or atomic-save implementation.
class TestFile {
 public:
  bool open(const fs::path& path, bool write) {
    path_ = path;
    const auto decision = ff::before(ff::Operation::Open, 0, path_);
    last_error_ = decision.error;
    if (decision.injected) return false;
    stream_.open(path_, (write ? std::ios::out : std::ios::in) |
                            std::ios::binary |
                            (write ? std::ios::trunc : std::ios::openmode{}));
    return stream_.is_open();
  }

  bool read(std::size_t requested, slrr::test::Bytes& output,
            std::size_t& count) {
    const auto decision =
        ff::before(ff::Operation::Read, requested, path_);
    last_error_ = decision.error;
    count = 0;
    output.clear();
    if (decision.failure) return false;
    output.resize(static_cast<std::size_t>(decision.allowed_bytes));
    if (decision.allowed_bytes != 0) {
      stream_.read(reinterpret_cast<char*>(output.data()),
                   static_cast<std::streamsize>(decision.allowed_bytes));
      count = static_cast<std::size_t>(stream_.gcount());
      output.resize(count);
    }
    return count == decision.allowed_bytes;
  }

  bool write(const slrr::test::Bytes& input, std::size_t& count) {
    const auto decision =
        ff::before(ff::Operation::Write, input.size(), path_);
    last_error_ = decision.error;
    count = 0;
    if (decision.failure) return false;
    count = static_cast<std::size_t>(decision.allowed_bytes);
    if (count != 0) {
      stream_.write(reinterpret_cast<const char*>(input.data()),
                    static_cast<std::streamsize>(count));
    }
    if (decision.injected && count != input.size()) {
      stream_.setstate(std::ios::badbit);
      return false;
    }
    return static_cast<bool>(stream_);
  }

  bool flush() {
    const auto decision = ff::before(ff::Operation::Flush, 0, path_);
    last_error_ = decision.error;
    if (decision.injected) {
      stream_.setstate(std::ios::badbit);
      return false;
    }
    stream_.flush();
    return static_cast<bool>(stream_);
  }

  bool close() {
    const auto decision = ff::before(ff::Operation::Close, 0, path_);
    last_error_ = decision.error;
    stream_.close();
    if (decision.injected) {
      stream_.setstate(std::ios::badbit);
      return false;
    }
    return !stream_.fail();
  }

  std::error_code last_error() const { return last_error_; }

 private:
  fs::path path_;
  std::fstream stream_;
  std::error_code last_error_;
};

bool remove_file(const fs::path& path) {
  const auto decision = ff::before(ff::Operation::Delete, 0, path);
  if (decision.injected) return false;
  std::error_code error;
  fs::remove_all(path, error);
  return !error;
}

bool copy_with_faults(const fs::path& source, const fs::path& destination) {
  const auto decision =
      ff::before(ff::Operation::Copy, 0, source, destination);
  if (decision.injected) return false;
  std::error_code error;
  fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
  return !error;
}

bool rename_file(const fs::path& source, const fs::path& destination) {
  const auto decision =
      ff::before(ff::Operation::Rename, 0, source, destination);
  if (decision.injected) return false;
  std::error_code error;
  fs::rename(source, destination, error);
  return !error;
}

slrr::test::Bytes read_whole(const fs::path& path) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  SLRR_REQUIRE(!error);
  TestFile file;
  SLRR_REQUIRE(file.open(path, false));
  slrr::test::Bytes contents;
  std::size_t count = 0;
  SLRR_REQUIRE(file.read(static_cast<std::size_t>(size), contents, count));
  SLRR_REQUIRE(file.close());
  return contents;
}

std::int64_t fault_metric() {
  const auto snapshot = slrr::metrics::snapshot();
  const auto found = std::find_if(
      snapshot.values.begin(), snapshot.values.end(), [](const auto& value) {
        return value.name == "host.file_faults_injected";
      });
  return found == snapshot.values.end() ? 0 : found->value;
}

}  // namespace

SLRR_TEST(file_faults_no_injection_baseline_covers_every_operation) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-baseline");
  const auto source = temporary.path() / "source.sdat";
  const auto copied = temporary.path() / "copied.sdat";
  const auto moved = temporary.path() / "moved.sdat";
  slrr::test::write_binary_file(source, bytes("abcdefgh"));
  SLRR_REQUIRE(read_whole(source) == bytes("abcdefgh"));

  const auto output = temporary.path() / "output.sdat";
  TestFile file;
  SLRR_REQUIRE(file.open(output, true));
  std::size_t count = 0;
  SLRR_REQUIRE(file.write(bytes("payload"), count));
  SLRR_REQUIRE(count == 7);
  SLRR_REQUIRE(file.flush());
  SLRR_REQUIRE(file.close());
  SLRR_REQUIRE(slrr::test::read_binary_file(output) == bytes("payload"));
  SLRR_REQUIRE(copy_with_faults(source, copied));
  SLRR_REQUIRE(rename_file(copied, moved));
  SLRR_REQUIRE(slrr::test::read_binary_file(moved) == bytes("abcdefgh"));
  SLRR_REQUIRE(remove_file(moved));
  SLRR_REQUIRE(!fs::exists(moved));
  SLRR_REQUIRE(fault_metric() == 0);
}

SLRR_TEST(file_faults_open_permission_threshold_and_path_scope) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-open");
  const auto target = temporary.path() / "target.sdat";
  const auto other = temporary.path() / "other.sdat";
  slrr::test::write_binary_file(target, bytes("target"));
  slrr::test::write_binary_file(other, bytes("other"));
  SLRR_REQUIRE(ff::set_rule(rule(ff::Operation::Open,
                                 ff::Fault::PermissionDenied, 1, target)));
  SLRR_REQUIRE(read_whole(target) == bytes("target"));
  TestFile blocked;
  SLRR_REQUIRE(!blocked.open(target, false));
  SLRR_REQUIRE(blocked.last_error() == std::errc::permission_denied);
  SLRR_REQUIRE(read_whole(other) == bytes("other"));
}

SLRR_TEST(file_faults_read_short_eof_and_permission_modes) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-read");
  const auto target = temporary.path() / "read.sdat";
  slrr::test::write_binary_file(target, bytes("abcdefgh"));
  auto short_rule = rule(ff::Operation::Read, ff::Fault::ShortRead, 1, target);
  short_rule.short_bytes = 3;
  SLRR_REQUIRE(ff::set_rule(short_rule));
  SLRR_REQUIRE(read_whole(target) == bytes("abcdefgh"));
  SLRR_REQUIRE(read_whole(target) == bytes("abc"));
  SLRR_REQUIRE(read_whole(target) == bytes("abc"));

  ff::reset();
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Read, ff::Fault::EndOfFile, 0, target)));
  SLRR_REQUIRE(read_whole(target).empty());

  ff::reset();
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Read, ff::Fault::PermissionDenied, 0, target)));
  TestFile file;
  SLRR_REQUIRE(file.open(target, false));
  slrr::test::Bytes output;
  std::size_t count = 99;
  SLRR_REQUIRE(!file.read(8, output, count));
  SLRR_REQUIRE(count == 0);
  SLRR_REQUIRE(file.last_error() == std::errc::permission_denied);
}

SLRR_TEST(file_faults_read_fail_after_byte_threshold) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-read-bytes");
  const auto target = temporary.path() / "read-bytes.sdat";
  slrr::test::write_binary_file(target, bytes("abcdefgh"));
  auto short_rule = byte_rule(ff::Operation::Read, ff::Fault::ShortRead, 3, target);
  short_rule.short_bytes = 1;
  SLRR_REQUIRE(ff::set_rule(short_rule));
  TestFile file;
  SLRR_REQUIRE(file.open(target, false));
  slrr::test::Bytes output;
  std::size_t count = 0;
  SLRR_REQUIRE(file.read(8, output, count));
  SLRR_REQUIRE(output == bytes("abc"));
  SLRR_REQUIRE(file.read(5, output, count));
  SLRR_REQUIRE(output == bytes("d"));

  ff::reset();
  SLRR_REQUIRE(ff::set_rule(byte_rule(
      ff::Operation::Read, ff::Fault::PermissionDenied, 4, target)));
  TestFile blocked;
  SLRR_REQUIRE(blocked.open(target, false));
  SLRR_REQUIRE(blocked.read(8, output, count));
  SLRR_REQUIRE(output == bytes("abcd"));
  SLRR_REQUIRE(!blocked.read(4, output, count));
  SLRR_REQUIRE(count == 0);
}

SLRR_TEST(file_faults_write_partial_and_disk_full_operation_threshold) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-write");
  const auto target = temporary.path() / "write.sdat";
  auto partial = rule(ff::Operation::Write, ff::Fault::PartialWrite, 1, target);
  partial.short_bytes = 3;
  SLRR_REQUIRE(ff::set_rule(partial));
  TestFile first;
  SLRR_REQUIRE(first.open(target, true));
  std::size_t count = 0;
  SLRR_REQUIRE(first.write(bytes("abcdef"), count));
  SLRR_REQUIRE(count == 6);
  SLRR_REQUIRE(first.close());
  TestFile second;
  SLRR_REQUIRE(second.open(target, true));
  SLRR_REQUIRE(!second.write(bytes("abcdef"), count));
  SLRR_REQUIRE(count == 3);
  (void)second.close();
  SLRR_REQUIRE(slrr::test::read_binary_file(target) == bytes("abc"));

  ff::reset();
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Write, ff::Fault::DiskFull, 0, target)));
  TestFile full;
  SLRR_REQUIRE(full.open(target, true));
  SLRR_REQUIRE(!full.write(bytes("abcdef"), count));
  SLRR_REQUIRE(count == 0);
  SLRR_REQUIRE(full.last_error() == std::errc::no_space_on_device);
  SLRR_REQUIRE(slrr::test::read_binary_file(target).empty());
}

SLRR_TEST(file_faults_write_fail_after_byte_threshold) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-write-bytes");
  const auto target = temporary.path() / "write-bytes.sdat";
  auto partial = byte_rule(ff::Operation::Write, ff::Fault::PartialWrite, 4, target);
  partial.short_bytes = 2;
  SLRR_REQUIRE(ff::set_rule(partial));
  TestFile file;
  SLRR_REQUIRE(file.open(target, true));
  std::size_t count = 0;
  SLRR_REQUIRE(!file.write(bytes("abcdefgh"), count));
  SLRR_REQUIRE(count == 4);
  (void)file.close();
  SLRR_REQUIRE(slrr::test::read_binary_file(target) == bytes("abcd"));

  ff::reset();
  SLRR_REQUIRE(ff::set_rule(
      byte_rule(ff::Operation::Write, ff::Fault::DiskFull, 4, target)));
  TestFile full;
  SLRR_REQUIRE(full.open(target, true));
  SLRR_REQUIRE(!full.write(bytes("abcdefgh"), count));
  SLRR_REQUIRE(count == 4);
  SLRR_REQUIRE(!full.write(bytes("ijkl"), count));
  SLRR_REQUIRE(count == 0);
  SLRR_REQUIRE(full.last_error() == std::errc::no_space_on_device);
}

SLRR_TEST(file_faults_flush_permission_after_operation_threshold) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-flush");
  const auto target = temporary.path() / "flush.sdat";
  SLRR_REQUIRE(ff::set_rule(rule(ff::Operation::Flush,
                                 ff::Fault::PermissionDenied, 1, target)));
  TestFile first;
  SLRR_REQUIRE(first.open(target, true));
  std::size_t count = 0;
  SLRR_REQUIRE(first.write(bytes("one"), count));
  SLRR_REQUIRE(first.flush());
  SLRR_REQUIRE(first.close());
  TestFile second;
  SLRR_REQUIRE(second.open(target, true));
  SLRR_REQUIRE(second.write(bytes("two"), count));
  SLRR_REQUIRE(!second.flush());
  SLRR_REQUIRE(second.last_error() == std::errc::permission_denied);
}

SLRR_TEST(file_faults_close_io_error_after_operation_threshold) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-close");
  const auto target = temporary.path() / "close.sdat";
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Close, ff::Fault::IoError, 1, target)));
  TestFile first;
  SLRR_REQUIRE(first.open(target, true));
  std::size_t count = 0;
  SLRR_REQUIRE(first.write(bytes("one"), count));
  SLRR_REQUIRE(first.close());
  TestFile second;
  SLRR_REQUIRE(second.open(target, true));
  SLRR_REQUIRE(second.write(bytes("two"), count));
  SLRR_REQUIRE(!second.close());
  SLRR_REQUIRE(second.last_error() == std::errc::io_error);
  SLRR_REQUIRE(slrr::test::read_binary_file(target) == bytes("two"));
}

SLRR_TEST(file_faults_delete_copy_and_rename_fail_without_touching_sources) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-path-operations");
  const auto source = temporary.path() / "source.sdat";
  const auto destination = temporary.path() / "destination.sdat";
  slrr::test::write_binary_file(source, bytes("reference-payload"));

  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Copy, ff::Fault::DiskFull, 1, destination)));
  SLRR_REQUIRE(copy_with_faults(source, destination));
  const auto original_destination = slrr::test::read_binary_file(destination);
  SLRR_REQUIRE(!copy_with_faults(source, destination));
  SLRR_REQUIRE(slrr::test::read_binary_file(source) == bytes("reference-payload"));
  SLRR_REQUIRE(slrr::test::read_binary_file(destination) == original_destination);

  ff::reset();
  SLRR_REQUIRE(fs::remove(source));
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Rename, ff::Fault::PermissionDenied, 1, source)));
  SLRR_REQUIRE(rename_file(destination, source));
  SLRR_REQUIRE(!rename_file(source, destination));
  SLRR_REQUIRE(fs::exists(source));
  SLRR_REQUIRE(!fs::exists(destination));

  ff::reset();
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Delete, ff::Fault::PermissionDenied, 1, source)));
  SLRR_REQUIRE(remove_file(source));
  slrr::test::write_binary_file(source, bytes("recreated"));
  SLRR_REQUIRE(!remove_file(source));
  SLRR_REQUIRE(slrr::test::read_binary_file(source) == bytes("recreated"));
}

SLRR_TEST(file_faults_reset_and_nested_scopes_restore_rules_and_counters) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-scopes");
  const auto target = temporary.path() / "scoped.sdat";
  slrr::test::write_binary_file(target, bytes("abcdefgh"));
  auto outer_rule = rule(ff::Operation::Read, ff::Fault::ShortRead, 1, target);
  outer_rule.short_bytes = 2;
  {
    ff::ScopedRule outer(outer_rule);
    SLRR_REQUIRE(read_whole(target) == bytes("abcdefgh"));
    {
      ff::ScopedRule inner(
          rule(ff::Operation::Open, ff::Fault::PermissionDenied, 0, target));
      TestFile blocked;
      SLRR_REQUIRE(!blocked.open(target, false));
    }
    SLRR_REQUIRE(read_whole(target) == bytes("ab"));
    ff::reset();
    SLRR_REQUIRE(!ff::enabled());
    SLRR_REQUIRE(read_whole(target) == bytes("abcdefgh"));
  }
  SLRR_REQUIRE(!ff::enabled());
  SLRR_REQUIRE(!ff::set_rule(
      rule(ff::Operation::Write, ff::Fault::ShortRead, 0, target)));
  SLRR_REQUIRE(!ff::set_rule(
      rule(ff::Operation::Read, ff::Fault::PartialWrite, 0, target)));
}

SLRR_TEST(file_faults_diagnostic_metric_and_secret_exclusion) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-diagnostics");
  const auto trace = temporary.path() / "events.jsonl";
  std::string error;
  SLRR_REQUIRE(slrr::diagnostics::start(trace.string(), error, 8));
  SLRR_REQUIRE(error.empty());
  const auto target = temporary.path() / "secret-reference-path.sdat";
  auto partial = rule(ff::Operation::Write, ff::Fault::PartialWrite, 0, target);
  partial.short_bytes = 2;
  SLRR_REQUIRE(ff::set_rule(partial));
  TestFile file;
  SLRR_REQUIRE(file.open(target, true));
  std::size_t count = 0;
  SLRR_REQUIRE(!file.write(bytes("TOP-SECRET-FILE-CONTENTS"), count));
  SLRR_REQUIRE(count == 2);
  SLRR_REQUIRE(fault_metric() == 1);

  const auto events = slrr::diagnostics::snapshot();
  SLRR_REQUIRE(events.size() == 1);
  SLRR_REQUIRE(events[0].category == slrr::diagnostics::Category::File);
  SLRR_REQUIRE(events[0].name == "file.fault.injected");
  SLRR_REQUIRE(events[0].method == "write");
  SLRR_REQUIRE(events[0].error == "partial_write");
  SLRR_REQUIRE(events[0].resource.empty());
  SLRR_REQUIRE(events[0].detail.empty());
  slrr::diagnostics::flush();
  slrr::diagnostics::shutdown();
  const auto json = slrr::test::read_text_file(trace);
  SLRR_REQUIRE(json.find("\"category\":\"file\"") != std::string::npos);
  SLRR_REQUIRE(json.find("secret-reference-path") == std::string::npos);
  SLRR_REQUIRE(json.find("TOP-SECRET-FILE-CONTENTS") == std::string::npos);
}

SLRR_TEST(file_faults_reference_save_fixture_is_byte_for_byte_preserved) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("files-faults-reference-save");
  const auto reference = temporary.path() / "existing-reference-career.sdat";
  const auto target = temporary.path() / "injected-career.sdat";
  const auto reference_bytes = bytes("SDAT-0102-existing-reference-save");
  slrr::test::write_binary_file(reference, reference_bytes);
  std::error_code error;
  const auto write_time = fs::last_write_time(reference, error);
  SLRR_REQUIRE(!error);

  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Open, ff::Fault::PermissionDenied, 0, target)));
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Write, ff::Fault::DiskFull, 0, target)));
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Delete, ff::Fault::PermissionDenied, 0, target)));
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Copy, ff::Fault::DiskFull, 0, target)));
  SLRR_REQUIRE(ff::set_rule(
      rule(ff::Operation::Rename, ff::Fault::PermissionDenied, 0, target)));

  TestFile blocked;
  SLRR_REQUIRE(!blocked.open(target, true));
  SLRR_REQUIRE(!copy_with_faults(reference, target));
  SLRR_REQUIRE(!rename_file(reference, target));
  SLRR_REQUIRE(!remove_file(target));
  SLRR_REQUIRE(slrr::test::read_binary_file(reference) == reference_bytes);
  const auto after_write_time = fs::last_write_time(reference, error);
  SLRR_REQUIRE(!error);
  SLRR_REQUIRE(after_write_time == write_time);
}
