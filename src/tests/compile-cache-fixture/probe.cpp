// SPDX-License-Identifier: Apache-2.0
//
// Never compiled. It exists so the fixture has a C++ compile edge, because a
// compiler launcher is only observable in the GENERATED buildsystem and a project
// with no target generates nothing to observe (#187). The stand-in launchers this
// is wired to are `cmake` and `ctest`, which would not compile it anyway.
namespace FastCacheFixture
{
inline int Probe() noexcept
{
    return 0;
}
} // namespace FastCacheFixture
