#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

SLRR_TEST(registration_and_assertions)
{
    const std::string registered("registered");
    SLRR_REQUIRE(!registered.empty());
}

SLRR_TEST(malformed_binary_fixtures)
{
    slrr::test::TemporaryDirectory temporary("smoke-binary");
    const slrr::test::Bytes malformed{0x52, 0x50, 0x4b, 0x00, 0xff, 0x7f};
    const auto malformed_path = temporary.path() / "malformed.rpk";

    slrr::test::write_binary_file(malformed_path, malformed);
    SLRR_REQUIRE(slrr::test::read_binary_file(malformed_path) == malformed);

    const auto truncated_path = temporary.path() / "truncated.rpk";
    slrr::test::write_truncated_file(truncated_path, malformed, 3);
    const slrr::test::Bytes expected{0x52, 0x50, 0x4b};
    SLRR_REQUIRE(slrr::test::read_binary_file(truncated_path) == expected);
}

SLRR_TEST(malformed_text_fixtures)
{
    slrr::test::TemporaryDirectory temporary("smoke-text");
    const std::string malformed("[unterminated\nvalue=\xff");
    const auto path = temporary.path() / "malformed.cfg";

    slrr::test::write_text_file(path, malformed);
    SLRR_REQUIRE(slrr::test::read_text_file(path) == malformed);
}

SLRR_TEST(temporary_fixtures_clean_up)
{
    std::filesystem::path temporary_path;
    {
        slrr::test::TemporaryDirectory temporary("smoke-cleanup");
        temporary_path = temporary.path();
        slrr::test::write_text_file(temporary_path / "temporary.txt", "temporary");
        SLRR_REQUIRE(std::filesystem::exists(temporary_path));
    }
    SLRR_REQUIRE(!std::filesystem::exists(temporary_path));
}
