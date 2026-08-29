#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Exercise scripts/ci-scope.sh against a throwaway repository.
#
# Registered as `ctest -R ci-scope`. It is NOT labelled `smoke`: it needs no
# daemon, no socket and no compiler -- only git, which every checkout already
# has -- so it belongs in the default set beside repository-hygiene and
# net-boundary.
#
# What it is here to stop: the classifier is what decides whether a pull request
# compiles anything at all, so a table that quietly starts answering `code=false`
# for a source path is a merge that no job ever built, on a pull request whose
# required checks all reported green. Both directions are therefore asserted, and
# so is every way of not knowing.

set -euo pipefail

SCOPE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ci-scope.sh"
[[ -x "$SCOPE" ]] || { echo "ci-scope-test: $SCOPE is not executable"; exit 1; }
command -v git >/dev/null || { echo "ci-scope-test: git is required"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# A repository of our own, so the test says nothing about the tree it runs in --
# and so it still works from an exported tarball or a worktree whose git this
# host cannot read.
export GIT_DIR= GIT_WORK_TREE= GIT_INDEX_FILE=
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
cd "$WORK"
git init -q -b master .
git config user.email ci-scope@example.invalid
git config user.name "ci-scope test"

mkdir -p docs/guide .agent/rules src/FastCache/Core scripts cmake packaging .github/workflows
for f in docs/guide/page.md .agent/rules/thing.md README.md mkdocs.yml CMakeLists.txt \
         src/FastCache/Core/Thing.cpp scripts/tidy-sweep.sh cmake/Version.cmake \
         packaging/CMakeLists.txt .github/workflows/build.yml; do
    echo original > "$f"
done
git add -A
git commit -q -m base
BASE="$(git rev-parse HEAD)"

failures=0
Expect() { # $1 = expected value, $2 = label, $3.. = files to touch
    local want="$1" label="$2"; shift 2
    local f
    for f in "$@"; do echo changed > "$f"; done
    git add -A && git commit -q -m "$label"
    local got
    got="$("$SCOPE" "$BASE" HEAD 2>/dev/null || echo "code=ERROR")"
    if [[ "$got" == "$want" ]]; then
        printf '  ok    %-44s %s\n' "$label" "$got"
    else
        printf '  FAIL  %-44s got %s, want %s\n' "$label" "$got" "$want"
        failures=$((failures + 1))
    fi
    git reset -q --hard "$BASE"
}

echo "documentation only -- the matrix must be skippable:"
Expect "code=false" "docs/"                docs/guide/page.md
Expect "code=false" ".agent/"              .agent/rules/thing.md
Expect "code=false" "a root .md"           README.md
Expect "code=false" "several doc paths"    docs/guide/page.md README.md .agent/rules/thing.md

echo "anything that reaches a build -- the matrix must run:"
Expect "code=true"  "a source file"        src/FastCache/Core/Thing.cpp
Expect "code=true"  "scripts/"             scripts/tidy-sweep.sh
Expect "code=true"  "the workflow itself"  .github/workflows/build.yml
Expect "code=true"  "cmake/"               cmake/Version.cmake
Expect "code=true"  "CMakeLists.txt"       CMakeLists.txt
Expect "code=true"  "packaging/"           packaging/CMakeLists.txt
Expect "code=true"  "mkdocs.yml"           mkdocs.yml
Expect "code=true"  "docs AND code"        docs/guide/page.md src/FastCache/Core/Thing.cpp

echo "a path no row describes is code, never documentation:"
echo new > unheard-of.frobnicate
git add -A && git commit -q -m unknown
got="$("$SCOPE" "$BASE" HEAD 2>/dev/null || echo code=ERROR)"
if [[ "$got" == "code=true" ]]; then
    printf '  ok    %-44s %s\n' "an unrecognised extension" "$got"
else
    printf '  FAIL  %-44s got %s, want code=true\n' "an unrecognised extension" "$got"
    failures=$((failures + 1))
fi
git reset -q --hard "$BASE"

echo "every way of not knowing escalates:"
for spec in "unresolvable base|0000000000000000000000000000000000000000|HEAD" \
            "unresolvable head|${BASE}|0000000000000000000000000000000000000000" \
            "an empty diff|${BASE}|${BASE}"; do
    IFS='|' read -r label b h <<< "$spec"
    got="$("$SCOPE" "$b" "$h" 2>/dev/null || echo code=ERROR)"
    if [[ "$got" == "code=true" ]]; then
        printf '  ok    %-44s %s\n' "$label" "$got"
    else
        printf '  FAIL  %-44s got %s, want code=true\n' "$label" "$got"
        failures=$((failures + 1))
    fi
done

if [[ "$failures" -ne 0 ]]; then
    echo "ci-scope: ${failures} case(s) failed"
    exit 1
fi
echo "ci-scope: every case classified correctly"
