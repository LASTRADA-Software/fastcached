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
    local roots root alternation=""
    roots="$(third_party_roots "$1")" || return 2
    while IFS= read -r root; do
        root="$(printf '%s' "$root" | sed 's/[]$*+?(){}|.^[]/\\&/g')"
        alternation="${alternation:+${alternation}|}${root}"
    done <<< "$roots"
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

# One line naming what an enumerator declined, or nothing when it declined nothing.
#
# @param 1 What the enumerator lists, for the sentence: `shell script(s)`.
# @param 2 The declined paths, newline-separated, possibly empty.
third_party_declined_summary() {
    [ -n "${2-}" ] || return 0
    printf 'declined %s third-party %s under the roots in scripts/lib/third-party-roots.txt, first %s\n' \
        "$(grep -c . <<< "$2")" "$1" "${2%%$'\n'*}"
}

# @param 1 `-v` to keep what is NOT under a root, empty to keep what is.
# @param 2 The repository root. @param 3 The paths.
_third_party_select() {
    local pattern selected status=0
    pattern="$(third_party_path_pattern "$2")" || return 2
    [ -n "$3" ] || return 0
    selected="$(grep -E ${1:+"$1"} -- "$pattern" <<< "$3")" || status=$?
    # grep answers 1 for "selected nothing", which is an answer; anything above it is
    # the instrument failing, and must not read as an empty selection.
    [ "$status" -le 1 ] || return 2
    [ -z "$selected" ] || printf '%s\n' "$selected"
}
