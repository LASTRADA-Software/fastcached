# SPDX-License-Identifier: Apache-2.0
#
# `scratch_git`: git for a repository a FIXTURE created, never the one the caller happens to
# be running inside. Sourced, never run.
#
# A self-test stages a tree and makes it a repository -- `git init`, `git add`, a commit --
# because `git ls-files` is the enumeration the check under test uses. Every one of those
# commands obeys `GIT_DIR`, `GIT_WORK_TREE` and the rest of
# `scripts/lib/git-scrub-variables.txt` when they are in the ENVIRONMENT, and then writes
# the repository they name instead of the scratch tree: an exported `GIT_DIR` around a ctest
# run made every scratch `git init` write `core.worktree` into the configuration all of this
# repository's worktrees share (2026-10-02). So the scrub is folded into the command rather
# than called beside it: `scratch_git` is how a fixture spells git, and `scratch-git` in the
# default ctest set refuses a scratch write spelled any other way.
#
# The scrub is per COMMAND, in a subshell, so the caller's own environment is untouched: the
# check under test, which a self-test runs over the staged tree, still sees whatever it was
# given. That is deliberate -- a READ under a hijacked `GIT_DIR` answers about the wrong index
# and fails the case loudly, which is a red somebody sees; only the WRITES did damage.
#
# bash 3.2, because a hygiene script `ctest` runs is constrained to it.

# Every name in git-scrub-variables.txt, space-separated, read once at source time.
# Empty when the file is missing, unreadable, names nothing, or names something that is not a
# `GIT_*` variable -- and `scratch_git` then refuses rather than running git unscrubbed.
ScratchGitScrubbed=""
_scratch_git_read_names() {
    local file="$1" line names=""
    [ -r "$file" ] || return 2
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -n "$line" ] || continue
        case "$line" in
            GIT_*) ;;
            *) return 2 ;;
        esac
        case "$line" in
            *[!A-Z_]*) return 2 ;;
        esac
        names="${names:+${names} }${line}"
    done < "$file"
    [ -n "$names" ] || return 2
    printf '%s\n' "$names"
}
ScratchGitScrubbed="$(_scratch_git_read_names "${BASH_SOURCE[0]%/*}/git-scrub-variables.txt")" \
    || ScratchGitScrubbed=""

# Run git with every variable git-scrub-variables.txt names removed from ITS environment.
#
# Refuses -- a message on stderr and status 2, git never started -- when the list could not be
# read: running git unscrubbed is the defect this exists to end, so there is no fallback.
#
# @param @ git's arguments, exactly as `git` would take them.
# @return git's exit status, or 2 when the scrub list could not be read.
scratch_git() {
    if [ -z "${ScratchGitScrubbed}" ]; then
        echo "scratch_git: ${BASH_SOURCE[0]%/*}/git-scrub-variables.txt could not be read as a list of GIT_* names, so git was NOT run -- unscrubbed, a fixture's write lands in whatever repository an inherited GIT_DIR names" >&2
        return 2
    fi
    # shellcheck disable=SC2086  # one variable name per word, validated above
    ( unset ${ScratchGitScrubbed}; exec git "$@" )
}

# Remove every variable git-scrub-variables.txt names from THIS shell's environment, for a
# script whose whole run happens in a repository it created -- so that what it runs, not only
# what it writes, answers about that repository. A self-test whose subject includes what the
# check under test does with an INHERITED variable does not want this. One that judges source
# text and spells git raw in its own controls does -- `check-scratch-git.sh --self-test` calls
# it, because an unscrubbed control under an exported GIT_DIR writes the caller's repository.
#
# @return 0, or 2 (nothing unset) when the list could not be read.
scrub_git_environment() {
    if [ -z "${ScratchGitScrubbed}" ]; then
        echo "scrub_git_environment: ${BASH_SOURCE[0]%/*}/git-scrub-variables.txt could not be read as a list of GIT_* names, so nothing was scrubbed" >&2
        return 2
    fi
    # shellcheck disable=SC2086  # one variable name per word, validated at source time
    unset ${ScratchGitScrubbed}
}
