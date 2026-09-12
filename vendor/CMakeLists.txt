# SPDX-License-Identifier: Apache-2.0
#
# The vendored endo TUI, built as one static library for fastcache-cli's
# `live-stats` view (#134).
#
# WHY THIS FILE EXISTS AT ALL, rather than `add_subdirectory(endo/tui)`:
# `endo/tui/CMakeLists.txt` is part of the VERBATIM copy and is deliberately not
# used. It names `endo-platform`, `coro`, `stb_image` and three endo-only helper
# functions (`set_pedantic_compiler_warnings`, `enable_sanitizers`,
# `enable_clang_tidy`), none of which exist here. Editing it to suit us would put
# a local change inside the contribution diff on day one, which is the whole thing
# `vendor/VENDOR.md` says we must not do. So it stays untouched and unread, and
# this file -- which is OURS -- states the target in fastcached's own terms.

# ----------------------------------------------------------------------------
# The source set, which is a CLOSURE rather than a hand-picked list.
#
# Upstream's own target has 40 translation units. We build 21: the transitive
# include closure of the components #134 needs, plus the platform terminal
# implementations that supply `tui/Terminal.hpp`, `tui/TerminalInput.hpp` and
# `tui/TerminalOutput.hpp` (those live under `platform/`, so they are not the
# `.cpp` sibling of any header in the closure and would be missed by it).
#
# The 19 left out are left out because they reach one of the two dependency
# groups this tree deliberately did not take:
#
#   stb_image   ImageLoader, ImageProvider, StbImageImpl, MarkdownRenderer
#               -- #134 loads no images.
#   coro        everything under runtime/, and QuestionComponent, which reaches
#               it through runtime/Modal.hpp -- fastcached has its own event
#               loop (Async/Task, IReactor) and endo's must not become a second
#               one. See VENDOR.md, "Two of some things".
#
# ...and the remainder (popups, completer, Markdown, LogPanel, InputField,
# KeyBindings, List, Spinner, StatusBar, SemanticBlockClient,
# GenericSyntaxHighlighter, CommandRegistry, Dialog) simply are not reached: they
# are an interactive editor's widgets, and a stats view calls none of them. They
# stay ON DISK, verbatim -- subsetting the build is not subsetting the copy, and
# a later ticket that wants one adds a line here rather than a new import.
set(_fcTuiSources
    endo/tui/Box.cpp
    endo/tui/Buffer.cpp
    endo/tui/Canvas.cpp
    endo/tui/Cell.cpp
    endo/tui/Component.cpp
    endo/tui/HoverState.cpp
    endo/tui/MockTerminalOutput.cpp
    endo/tui/Screen.cpp
    endo/tui/SgrBuilder.cpp
    endo/tui/Sixel.cpp
    endo/tui/StyledText.cpp
    endo/tui/Text.cpp
    endo/tui/Theme.cpp
    endo/tui/Tooltip.cpp
    endo/tui/TreeTableView.cpp
    endo/tui/Unicode.cpp
    endo/tui/VtParser.cpp
    endo/tui/platform/TerminalShared.cpp
)

if(WIN32)
    list(APPEND _fcTuiSources
        endo/tui/platform/TerminalWin32.cpp
        endo/tui/platform/TerminalInputWin32.cpp
        endo/tui/platform/TerminalOutputWin32.cpp
    )
else()
    list(APPEND _fcTuiSources
        endo/tui/platform/Terminal.cpp
        endo/tui/platform/TerminalInput.cpp
        endo/tui/platform/TerminalOutput.cpp
    )
endif()

# A source that has gone missing -- a re-sync that dropped a file, a path typo --
# is a link error a hundred lines later naming a symbol rather than a file. Asked
# here, where the answer is the filename. The list is asserted non-empty too: a
# `list(APPEND)` into a misspelled variable yields an empty target that builds
# perfectly and exports nothing, and two empty lists agree.
if(_fcTuiSources STREQUAL "")
    message(FATAL_ERROR "vendor: the TUI source list is empty, so the library would build nothing")
endif()
foreach(_src IN LISTS _fcTuiSources)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        message(FATAL_ERROR
            "vendor: ${_src} is named in the TUI source list but is not in the tree. "
            "If this followed an upstream re-sync, the file moved or was removed there: "
            "update this list and record the change in vendor/VENDOR.md, rather than "
            "deleting the line, which would silently drop whatever used it.")
    endif()
endforeach()

add_library(fastcache-tui STATIC ${_fcTuiSources})

# The include root is `vendor/endo`, which is what makes the copy verbatim
# possible: upstream spells its own includes `<tui/...>`, `<platform/...>` and
# `<crispy/...>`, so reproducing endo's `src/` layout under one root resolves
# every one of them with no edit to any vendored file.
#
# SYSTEM, so warnings raised inside these headers do not reach the first-party
# translation units that include them. This tree builds with -Wconversion -Wextra
# and PEDANTIC_COMPILER_WERROR ON for Windows presets, and third-party headers
# are not ours to fix.
target_include_directories(fastcache-tui SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/endo")

target_link_libraries(fastcache-tui PUBLIC unicode::unicode)
target_compile_features(fastcache-tui PUBLIC cxx_std_23)

# Belt AND braces, deliberately.
#
# This directory is added BEFORE `include(PedanticCompiler)` / `include(ClangTidy)`
# in the top-level CMakeLists, which is the mechanism yaml-cpp, Catch2, zstd and
# lz4 already rely on -- `add_compile_options()` and `CMAKE_CXX_CLANG_TIDY` are
# directory-scoped and reach only what is defined after them. That ordering is a
# property of a line in ANOTHER file, though, and nothing here would notice if it
# moved: the analyser would simply start failing the build inside upstream code,
# with `WarningsAsErrors: "*"` and naming rules this code has no reason to satisfy.
# So the target says it for itself as well, where a reader of THIS file can see it.
set_target_properties(fastcache-tui PROPERTIES
    CXX_CLANG_TIDY ""
    C_CLANG_TIDY ""
)
