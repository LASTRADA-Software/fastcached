// SPDX-License-Identifier: Apache-2.0
// Catch2 entry point for the fastcache-cc-tests binary.

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    return Catch::Session().run(argc, argv);
}
