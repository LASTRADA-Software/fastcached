# SPDX-License-Identifier: Apache-2.0
#
# This project's MSVC warning suppressions, handed to cmake/portable/PedanticCompiler.cmake, which
# owns the mechanism -- the condition they apply under, and that no row arrives without its
# rationale -- and none of the rows. Included just before `include(PedanticCompiler)` by the
# top-level CMakeLists.txt, and by scripts/check-pedantic-suppressions.cmake, so the check asks the
# module what it does with THESE rows rather than with a copy of them.
#
# A row is `<flag>|<rationale>` and exists for a warning that reports INTENDED behaviour with no
# source change that keeps the behaviour. A warning a source edit can address is fixed at the
# source, never added here, and never silenced with a `#pragma` at the site
# (.agent/rules/build-and-toolchain.md, "`PEDANTIC_COMPILER_WERROR` decides fatality").
#
# C4324: `alignas(CacheLineBytes)` on `InMemoryLruStorage`'s read counters pads the class, and so
# pads every struct that holds one by value (three test rigs, on arm64). The padding is the point
# -- the layout keeps the two counters off a shared cache line and was benchmarked -- and the only
# layouts that do not warn either give that up or fill the whole class by hand.
set(PEDANTIC_COMPILER_MSVC_SUPPRESSIONS
    "/wd4324|reports intended padding from alignas, and every site is a deliberate cache-line layout or holds one"
)
