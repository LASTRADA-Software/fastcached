#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Decide whether a change can affect anything the build matrix tests.
#
# Prints one `key=value` line per classification to stdout, and — when
# `$GITHUB_OUTPUT` is set — appends the same lines there, which is how the
# `changes` job in `.github/workflows/build.yml` publishes them.
#
#   code=true    something that can change a compiled artefact, a test, a
#                package or the workflow itself was touched
#   code=false   every changed path is documentation this build never reads
#
# Usage:
#   scripts/ci-scope.sh <baseRef> <headRef>
#
# ## Why a script with a table, and not `dorny/paths-filter`
#
# Two reasons. This repository derives file lists from `git diff --name-only`
# already (`scripts/tidy-sweep.sh`), so the mechanism is one people here have
# read; and `AGENT.md` asks for a stated justification before a new third-party
# dependency, which "we could have written eleven lines of shell" does not meet.
# It is also testable without a runner, which a marketplace action is not:
# `ctest -R ci-scope` runs the table below against fabricated file lists.
#
# ## The direction every unknown errs in
#
# A path this table does not recognise is CODE. That is not conservatism for its
# own sake — it is the same rule `scripts/tidy-sweep.sh` states for its own base
# ref: "we could not tell what changed" must never read as "nothing did". The
# cost of getting it wrong in that direction is a matrix run nobody needed. The
# cost of getting it wrong in the other direction is a merged change that no job
# ever compiled, on a repository whose required checks would all have reported
# green.
#
# So: an unresolvable diff, an empty diff, an unreadable ref, a path with no
# matching row — every one of them is `code=true`.

set -euo pipefail

fatal() { echo "ci-scope: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# The table. One glob per row: a changed path matching ANY row is documentation
# as far as this build is concerned. Everything else is code.
#
# What is deliberately NOT here, because each one does reach the build:
#   mkdocs.yml            the Documentation workflow builds it --strict
#   .github/**            the workflow is the thing being scoped
#   scripts/**            CI runs these
#   packaging/**          the package jobs read them
#   cmake/**, CMakeLists.txt, CMakePresets.json, vcpkg.json
#
# `.agent/**` is here and that is a judgement worth stating: it is the agent
# rulebook, read by people and sessions rather than by any job in this file. If a
# rule file ever generates something, it stops belonging on this list.
DocOnlyPatterns=(
    'docs/*'
    '.agent/*'
    '*.md'
    'LICENSE'
    'LICENSE.txt'
    '.github/ISSUE_TEMPLATE/*'
    '.github/PULL_REQUEST_TEMPLATE.md'
)

IsDocOnlyPath() {
    local path="$1" pattern
    for pattern in "${DocOnlyPatterns[@]}"; do
        # shellcheck disable=SC2053  # the glob on the right is the point
        [[ "$path" == $pattern ]] && return 0
    done
    return 1
}

Publish() {
    echo "$1"
    [[ -n "${GITHUB_OUTPUT:-}" ]] && echo "$1" >> "$GITHUB_OUTPUT"
    return 0
}

# ---------------------------------------------------------------------------

[[ $# -eq 2 ]] || fatal "usage: $(basename "${BASH_SOURCE[0]}") <baseRef> <headRef>"
base="$1"
head="$2"

# An unresolvable ref escalates rather than failing the job: a workflow that
# cannot tell what changed must still test everything, and a hard error here
# would instead take the whole matrix down over a checkout parameter.
if ! git rev-parse --verify --quiet "${base}^{commit}" >/dev/null \
   || ! git rev-parse --verify --quiet "${head}^{commit}" >/dev/null; then
    echo "ci-scope: cannot resolve ${base}...${head}; assuming everything changed" >&2
    Publish "code=true"
    exit 0
fi

# Three dots, for the reason the team-run guide gives at length: two-dot renders
# every commit the base gained since the branch forked as this branch's own
# change, which here would mean classifying somebody else's merge as our diff.
if ! changed="$(git diff --name-only "${base}...${head}" 2>/dev/null)"; then
    echo "ci-scope: git diff ${base}...${head} failed; assuming everything changed" >&2
    Publish "code=true"
    exit 0
fi

if [[ -z "$changed" ]]; then
    # No files is not "no code". A merge commit, a diff taken against the wrong
    # base, or a force-push mid-run all land here, and none of them is evidence
    # that nothing needs building.
    echo "ci-scope: empty diff for ${base}...${head}; assuming everything changed" >&2
    Publish "code=true"
    exit 0
fi

codePaths=()
while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    IsDocOnlyPath "$path" || codePaths+=("$path")
done <<< "$changed"

if [[ ${#codePaths[@]} -eq 0 ]]; then
    echo "ci-scope: documentation only ($(wc -l <<< "$changed") path(s)); the build matrix is skipped" >&2
    Publish "code=false"
    exit 0
fi

echo "ci-scope: ${#codePaths[@]} path(s) reach the build, first few:" >&2
printf '  %s\n' "${codePaths[@]:0:5}" >&2
Publish "code=true"
