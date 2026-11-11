#ifndef SLRR_TEST_FIXTURES_HPP
#define SLRR_TEST_FIXTURES_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slrr::test {

using Bytes = std::vector<std::uint8_t>;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view prefix);
    ~TemporaryDirectory();

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

void write_binary_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
void write_text_file(const std::filesystem::path& path, std::string_view text);
void write_truncated_file(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes,
    std::size_t retained_size);
Bytes read_binary_file(const std::filesystem::path& path);
std::string read_text_file(const std::filesystem::path& path);

} // namespace slrr::test

#endif
