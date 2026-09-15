// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/YamlReader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <tests/ScratchPath.hpp>

namespace
{

/// The directory is per PROCESS and removed with it, not a fixed path shared by
/// every test binary on the machine. It used to be `temp / "fastcached-test"`,
/// which two cases writing the same stem would collide on -- and which nothing
/// ever cleaned up, so it grew without bound. Static rather than per call so the
/// several files a case writes stay together.
std::filesystem::path WriteTempYaml(std::string_view stem, std::string_view content)
{
    static FastCache::Testing::ScratchDirectory const scratch { "fastcached-yaml-test" };
    auto path = scratch.Path() / (std::string { stem } + ".yaml");
    std::ofstream out { path };
    out << content;
    return path;
}

} // namespace

// ReadYamlSettings: the one door. It answers "what did this file say", with no
// idea what any of it means -- which is what lets one reader serve two binaries
// whose settings are entirely different tables. What a KEY means, and which value
// it accepts, is each binary's option table's question: the daemon's is asked in
// `CliOptions_test.cpp` and `ConfigMerge_test.cpp`, through the assembly.

TEST_CASE("YamlReader: settings arrive as keys, values and line numbers", "[config][yaml]")
{
    auto const path = WriteTempYaml("settings-basic", "alpha: one\nbeta: 2\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE(settings.has_value());
    REQUIRE(settings->size() == 2);
    CHECK((*settings)[0].key == "alpha");
    CHECK((*settings)[0].values == std::vector<std::string> { "one" });
    CHECK((*settings)[0].line == 1);
    CHECK((*settings)[1].key == "beta");
    // Numbers, booleans and quoted strings all arrive as text: the option table's
    // own parser is what turns a value into a type, and it is the same one the
    // command line reaches. A reader that pre-typed values would be a second
    // parser for every setting, disagreeing with the first at the edges.
    CHECK((*settings)[1].values == std::vector<std::string> { "2" });
    CHECK((*settings)[1].line == 2);
}

TEST_CASE("YamlReader: a sequence arrives as several values under one key", "[config][yaml]")
{
    auto const path = WriteTempYaml("settings-seq", "toolchain:\n  - /usr/bin/gcc\n  - /usr/bin/clang\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE(settings.has_value());
    REQUIRE(settings->size() == 1);
    CHECK((*settings)[0].values == std::vector<std::string> { "/usr/bin/gcc", "/usr/bin/clang" });
}

TEST_CASE("YamlReader: a key with nothing under it is kept, not dropped", "[config][yaml]")
{
    // Whether "named and given nothing" means anything is the caller's question --
    // for a list setting it legitimately means "empty this" -- so the reader must
    // not answer it by discarding the key.
    auto const path = WriteTempYaml("settings-empty-key", "toolchain:\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE(settings.has_value());
    REQUIRE(settings->size() == 1);
    CHECK((*settings)[0].key == "toolchain");
    CHECK((*settings)[0].values.empty());
}

TEST_CASE("YamlReader: a file of nothing but comments is a valid configuration", "[config][yaml]")
{
    // The reference configuration this project ships is exactly this shape, and an
    // operator who has not uncommented anything yet has a working file rather than
    // a broken one.
    auto const path = WriteTempYaml("settings-comments", "# nothing here\n# nor here\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE(settings.has_value());
    CHECK(settings->empty());
}

TEST_CASE("YamlReader: a nested sequence element is refused rather than flattened", "[config][yaml]")
{
    // A caller applies these through appliers that each take one string, so there
    // is no representation for a nested element. Dropping it silently would be a
    // setting an operator wrote and nothing read.
    auto const path = WriteTempYaml("settings-nested", "peers:\n  - id: one\n    at: x\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE_FALSE(settings.has_value());
    CHECK(settings.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("YamlReader: a key written twice is refused by name, not resolved", "[config][yaml]")
{
    // Neither answer a caller could give is one the operator can see: a scalar keeps
    // the LAST value and a list appends BOTH. Whichever was meant, the file says two
    // things, so the reader says which key and where.
    auto const path = WriteTempYaml("settings-duplicate", "alpha: one\nbeta: 2\nalpha: three\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE_FALSE(settings.has_value());
    CHECK(settings.error().code == FastCache::ConfigErrorCode::ParseError);
    CHECK(settings.error().field == "alpha");
    CHECK(settings.error().line == 3);
    CHECK(settings.error().context.contains("first at line 1"));
}

TEST_CASE("YamlReader: a key that is not a scalar is refused rather than thrown", "[config][yaml]")
{
    // Reading a sequence as text throws inside yaml-cpp, and nothing above the reader
    // catches it: measured on a 0.2.0-568 worker, this document ended the process
    // with an unhandled exception (0xC0000409) instead of naming the line.
    auto const path = WriteTempYaml("settings-sequence-key", "? [a, b]\n: 1\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE_FALSE(settings.has_value());
    CHECK(settings.error().code == FastCache::ConfigErrorCode::ParseError);
    CHECK(settings.error().context == "non-scalar key");
    CHECK(settings.error().line == 1);
}

TEST_CASE("YamlReader: a document that is not a map is refused", "[config][yaml]")
{
    auto const path = WriteTempYaml("settings-scalar-doc", "just a string\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE_FALSE(settings.has_value());
    CHECK(settings.error().code == FastCache::ConfigErrorCode::ParseError);
}

TEST_CASE("YamlReader: a file that is not there is FileNotFound, not ParseError", "[config][yaml]")
{
    // The code is what a caller switches on and what an operator is sent to look
    // at. `yaml-cpp` reports a missing file as a `YAML::Exception` carrying "bad
    // file: <path>", which arriving as ParseError sends somebody hunting for a
    // syntax mistake in a file that does not exist -- and a typo in `--config` is
    // the ordinary way to produce it.
    static FastCache::Testing::ScratchDirectory const scratch { "fastcached-yaml-missing" };
    auto const missing = scratch.Path() / "definitely-not-here.yaml";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    auto const settings = FastCache::ReadYamlSettings(missing);
    REQUIRE_FALSE(settings.has_value());
    CHECK(settings.error().code == FastCache::ConfigErrorCode::FileNotFound);
}

TEST_CASE("YamlReader: a malformed document reports the line it gave up on", "[config][yaml]")
{
    auto const path = WriteTempYaml("settings-malformed", "alpha: [1,\n");
    auto const settings = FastCache::ReadYamlSettings(path);

    REQUIRE_FALSE(settings.has_value());
    CHECK(settings.error().code == FastCache::ConfigErrorCode::ParseError);
    CHECK(settings.error().line > 0);
}
