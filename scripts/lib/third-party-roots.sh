# SPDX-License-Identifier: Apache-2.0
#
# The bash reader of `scripts/lib/third-party-roots.txt` (#1370): which directories of
# the repository hold third-party source. Sourced, never run. The file carries the
# format and the reasons; this carries only the reading.
#
# bash 3.2, because a hygiene script `ctest` runs is constrained to it: no `mapfile`, no
# `declare -A`, no `${var^^}`.

# Print every third-party root, one per line, relative to @p 1.
#
# Refuses -- a message on stderr and status 2, never an empty answer -- when the file is
# missing or unreadable, names no root, or names one spelled outside the format (a
# leading or trailing `/`, `..`, or a backslash). An enumerator handed an empty answer
# would take every vendored file as this project's own, which is the defect this file
# exists to end.
#
# @param 1 The repository root.
third_party_roots() {
    local roots_file="$1/scripts/lib/third-party-roots.txt" line count=0
    if [ ! -r "$roots_file" ]; then
        echo "third-party roots: ${roots_file} is missing or unreadable, so no enumerator can tell third-party files from this project's own" >&2
        return 2
    fi
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -n "$line" ] || continue
        case "$line" in
            /* | */ | *..* | *\\*)
                echo "third-party roots: ${roots_file} names '${line}', which is not a root relative to the repository (no leading or trailing '/', no '..', no backslash)" >&2
                return 2
                ;;
        esac
        printf '%s\n' "$line"
        count=$((count + 1))
    done < "$roots_file"
    if [ "$count" -eq 0 ]; then
        echo "third-party roots: ${roots_file} names no root; refused rather than read as 'nothing is third-party'" >&2
        return 2
    fi
}

# An extended regular expression matching a repository-relative path under any
# third-party root, for `grep -E`: `^(vendor)(/|$)`. Refuses exactly as `third_party_roots`.
#
# The roots are path names, not patterns: every ERE metacharacter in one is escaped, so
# `a.b` does not match `aXb` and a `+` or `(` in a directory name is not a syntax error.
#
# @param 1 The repository root.
third_party_path_pattern() {
    local roots root escaped status alternation=""
    roots="$(third_party_roots "$1")" || return 2
    # Each escape is CHECKED: a sed that fails or is killed prints nothing, and an empty
    # root made the pattern `^()(/|$)`, which selects no path -- every vendored file read as
    # this project's own, with status 0. That fails OPEN, so it is refused by name, as the
    # selection's grep is. The roots arrive through process substitution, never a
    # herestring (see `_third_party_select`).
    while IFS= read -r root; do
        escaped="$(printf '%s' "$root" | sed 's/[]$*+?(){}|.^[]/\\&/g')" && status=0 || status=$?
        if [ "$status" -ne 0 ] || [ -z "$escaped" ]; then
            echo "third-party roots: sed exited ${status} escaping the root '${root}' of ${1}, so which files are third-party is not known -- this is the CHECK failing, not a verdict about the tree" >&2
            return 2
        fi
        alternation="${alternation:+${alternation}|}${escaped}"
    done < <(printf '%s\n' "$roots")
    printf '^(%s)(/|$)\n' "$alternation"
}

# The paths of @p 2 that are NOT under a third-party root of @p 1, one per line: what an
# enumerator keeps. Refuses exactly as `third_party_roots`, printing nothing -- and it
# reads the roots even for an empty list, so a missing file is refused on a tree that
# happened to list nothing.
#
# @param 1 The repository root. @param 2 Repository-relative paths, newline-separated.
first_party_paths() {
    _third_party_select -v "$1" "${2-}"
}

# The paths of @p 2 that ARE under a third-party root of @p 1: what an enumerator declines.
# Refuses exactly as `first_party_paths`.
#
# @param 1 The repository root. @param 2 Repository-relative paths, newline-separated.
third_party_paths() {
    _third_party_select "" "$1" "${2-}"
}

# Run a FILTER command over @p 1's bytes and answer with the FILTER's own status: the one way
# this library and `header-filter.sh` hand a string to an external program.
#
# A PIPE, and the filter is a SIBLING of the writer. Never `filter < <(printf ...)`: under that
# shape the filter process is the PARENT of the process-substitution writer, and twice on a
# GitHub Windows runner such a grep came back killed -- round 8 silently, round 10 as exit 148,
# which is 128 + 20, and 20 is SIGCHLD in the MSYS2 runtime (cygwin/signal.h; `kill -l 20`
# answers CHLD). That SIGCHLD is how it died is INFERRED: neither shape reproduced locally (see
# #1630). The parentage is MEASURED, each writer reading its own PPID out of /proc, on Git Bash
# 5.2.37 and on bash 3.2.57: the filter is the writer's parent for `cmd < <(w)`, for
# `$(cmd < <(w))`, and for an ARGUMENT `cmd <(w)` inside `$( )` or a pipeline; a top-level
# `cmd <(w)` is a sibling, and `done < <(cmd <(w))` is a parent on 5.2 and a sibling on 3.2. A
# pipe's writer is a child of the shell running the pipeline in every one of those contexts.
# Never a herestring either, which deadlocks at 64 KiB on Git Bash (#1591): a pipe's reader drains
# it concurrently. `PIPESTATUS[1]` is the filter's status whatever `pipefail` says -- under it, a
# `grep -q` that stops reading early would otherwise answer with the writer's SIGPIPE. The
# pipeline sits in an `&& ... ||` list so a caller's `set -e` cannot end the shell on the
# filter's ordinary 1 before the status is read.
#
# @param 1 The bytes, verbatim: a caller wanting a final newline passes one.
# @param @ The filter and its arguments.
# @return The filter's exit status; its output on stdout.
pipe_lines_into() {
    local text="$1"
    shift
    printf '%s' "$text" | "$@" && return 0 || return "${PIPESTATUS[1]}"
}

# `pipe_lines_into` for a filter of TWO inputs -- `comm`, `diff` -- which `cmd <(a) <(b)` fed
# before #1630, and which is that shape as an argument: the filter is the writers' parent inside
# `$( )` or a pipeline, which is where every such site in this tree sat. @p 1 arrives on file
# descriptor 3 and @p 2 on stdin, both through pipes whose writers are this shell's children, so
# the caller names them `/dev/fd/3` and `-`:
#   pipe_pair_into "$a"$'\n' "$b"$'\n' comm -23 /dev/fd/3 -
# `/dev/fd/3` is a requirement on the host: `echo <(true)` answers `/dev/fd/63` on Git Bash and
# on Linux bash 5 and 3.2 (measured), and macOS was not measured. Running the `<(...)` this
# replaced proves nothing about it, since bash falls back to named pipes where `/dev/fd` is
# absent. What guards a host without it is the `pipe_pair_into` case of
# check-tidy-header-filter.sh's self-test, which feeds both descriptors and fails CLOSED when
# either one does not arrive. Both are drained
# concurrently, so a filter that reads one input to its end before the other -- `diff` does --
# cannot deadlock on the other's 64 KiB.
#
# @param 1 The first input's bytes, verbatim. @param 2 The second's.
# @param @ The filter and its arguments, naming `/dev/fd/3` and `-`.
# @return The filter's exit status; its output on stdout.
pipe_pair_into() {
    local first="$1" second="$2"
    shift 2
    printf '%s' "$first" | { pipe_lines_into "$second" "$@"; } 3<&0 && return 0 || return "${PIPESTATUS[1]}"
}

# One line naming what an enumerator declined, or nothing when it declined nothing.
#
# @param 1 What the enumerator lists, for the sentence: `shell script(s)`.
# @param 2 The declined paths, newline-separated, possibly empty.
third_party_declined_summary() {
    [ -n "${2-}" ] || return 0
    printf 'declined %s third-party %s under the roots in scripts/lib/third-party-roots.txt, first %s\n' \
        "$(pipe_lines_into "$2"$'\n' grep -c .)" "$1" "${2%%$'\n'*}"
}

# @param 1 `-v` to keep what is NOT under a root, empty to keep what is.
# @param 2 The repository root. @param 3 The paths.
_third_party_select() {
    local pattern selected status=0
    pattern="$(third_party_path_pattern "$2")" || return 2
    [ -n "$3" ] || return 0
    # Through `pipe_lines_into`, never a herestring: Git Bash's bash implements `<<<` with a
    # PIPE and writes the whole string before it starts the reader, so an operand of 65536
    # bytes or more deadlocks -- measured on bash 5.2.37 to the byte, 65535 completes, 65536
    # hangs, deterministic 5/5 -- and it is SIZE, not content. This helper takes the WHOLE
    # tracked-file listing, which was 65028 bytes here when that was measured. Nor process
    # substitution, which made grep the writer's parent (`pipe_lines_into` says why that
    # matters). A pipe's reader drains it concurrently, so no size deadlocks, and the helper
    # answers with grep's own status.
    selected="$(pipe_lines_into "$3"$'\n' grep -E ${1:+"$1"} -- "$pattern")" || status=$?
    # grep answers 1 for "selected nothing", which is an answer; anything above it is
    # the instrument failing, and must not read as an empty selection. And it SAYS so:
    # a grep killed by a signal prints nothing, and this was the one refusal on the path
    # that did not, so a caller reported "status 2 and []" -- an outcome nobody could
    # attribute (round 8, tidy-header-filter-selftest on Windows-cl-debug, once in five
    # runs of an unchanged tree).
    if [ "$status" -gt 1 ]; then
        echo "third-party roots: grep exited ${status} selecting paths against the roots of ${2}, so which files are this project's own is not known -- this is the CHECK failing, not a verdict about the tree" >&2
        return 2
    fi
    [ -z "$selected" ] || printf '%s\n' "$selected"
}
