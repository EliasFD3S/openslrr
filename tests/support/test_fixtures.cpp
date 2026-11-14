#include "support/test_fixtures.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace slrr::test {
namespace {

std::filesystem::path make_temporary_path(std::string_view prefix)
{
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path()
        / ("slrr-" + std::string(prefix) + '-' + std::to_string(ticks) + '-' + std::to_string(id));
}

std::ofstream open_output(const std::filesystem::path& path, std::ios::openmode mode)
{
    std::ofstream stream(path, mode);
    if (!stream) {
        throw std::runtime_error("Unable to create test fixture: " + path.string());
    }
    return stream;
}

std::ifstream open_input(const std::filesystem::path& path, std::ios::openmode mode)
{
    std::ifstream stream(path, mode);
    if (!stream) {
        throw std::runtime_error("Unable to read test fixture: " + path.string());
    }
    return stream;
}

} // namespace

TemporaryDirectory::TemporaryDirectory(std::string_view prefix)
    : path_(make_temporary_path(prefix))
{
    if (!std::filesystem::create_directory(path_)) {
        throw std::runtime_error("Unable to create temporary test directory: " + path_.string());
    }
}

TemporaryDirectory::~TemporaryDirectory()
{
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

const std::filesystem::path& TemporaryDirectory::path() const noexcept
{
    return path_;
}

void write_binary_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    auto stream = open_output(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        throw std::runtime_error("Unable to write binary test fixture: " + path.string());
    }
}

void write_text_file(const std::filesystem::path& path, std::string_view text)
{
    auto stream = open_output(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("Unable to write text test fixture: " + path.string());
    }
}

void write_truncated_file(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes,
    std::size_t retained_size)
{
    if (retained_size > bytes.size()) {
        throw std::invalid_argument("Truncated fixture size exceeds the source size");
    }
    write_binary_file(path, bytes.first(retained_size));
}

Bytes read_binary_file(const std::filesystem::path& path)
{
    auto stream = open_input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string read_text_file(const std::filesystem::path& path)
{
    auto stream = open_input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace slrr::test
