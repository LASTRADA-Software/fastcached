#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Make a linked worktree readable by BOTH gits on this machine.
#
# ---------------------------------------------------------------------------
# The problem, and why the documented remedy was not one
# ---------------------------------------------------------------------------
#
# `git worktree add` writes ABSOLUTE pointers, in the spelling of whichever git
# created the worktree. A worktree created from Windows therefore holds
#
#     <worktree>/.git                      gitdir: D:/repo/.git/worktrees/<name>
#     <repo>/.git/worktrees/<name>/gitdir  D:/worktrees/<name>/.git
#
# and WSL git, which has no `D:`, resolves that against the CURRENT directory and
# reports
#
#     fatal: not a git repository: /mnt/d/worktrees/<name>/D:/repo/.git/worktrees/<name>
#
# -- measured, and note the shape: the two paths are CONCATENATED, so the error
# names a path that exists nowhere and reads like a corrupt repository rather than
# a pointer in the wrong dialect.
#
# `scripts/local-gate.sh` is the only thing that exercises GCC before CI does, and
# it needs git for its formatter list, its header-filter census and its test
# registration walk. In such a worktree every one of those answers NOTHING, so the
# gate refuses -- correctly since #1064, having previously passed -- and the
# compiler-specific defects it exists to catch reach CI every time (#336).
#
# **The documented remedy was "create the worktree from inside WSL", which is
# advice nobody in this workflow can take**: worktrees are created by the harness
# from the Windows side and a session does not choose its creating shell. A remedy
# only available in a worktree nobody has is what that ticket is about.
#
# ---------------------------------------------------------------------------
# What this does instead
# ---------------------------------------------------------------------------
#
# Rewrites both pointers RELATIVE, which every git resolves identically because
# neither drive letters nor mount points appear in them:
#
#     <worktree>/.git                      gitdir: ../../repo/.git/worktrees/<name>
#     <repo>/.git/worktrees/<name>/gitdir  ../../../../worktrees/<name>/.git
#
# **NOT `git worktree repair`**, which is the obvious answer and is the wrong
# direction: it rewrites the pointers ABSOLUTE in the dialect of whichever git runs
# it, so running it from Windows reproduces this state and running it from WSL
# breaks the Windows side instead. The property wanted here is that BOTH gits read
# the same tree, and only a relative pointer has it.
#
# **AND NEVER `git config core.worktree`.** That is not a stylistic preference: six
# scripts under `scripts/` run `git init` in synthetic trees, and with `GIT_DIR` and
# `GIT_WORK_TREE` exported into a `ctest` run those `git init` calls write
# `core.worktree` into the SHARED `.git/config` -- after which Windows git fails in
# EVERY worktree at once with `fatal: Invalid path '/mnt'` while WSL git keeps
# working. A lane did exactly that on this machine. Nothing here writes git CONFIG
# at all; it writes two pointer FILES and nothing else.
#
# Usage:
#   scripts/repair-worktree-pointers.sh                 diagnose this worktree
#   scripts/repair-worktree-pointers.sh --apply         and repair it
#   scripts/repair-worktree-pointers.sh --self-test
#
# Exit: 0 when the worktree is readable (already, or after `--apply`), 1 when it is
# not and this could not fix it. Diagnosing a broken tree WITHOUT `--apply` is also
# 1: the tree is still broken, and a status saying otherwise would be read as one
# that had been repaired.
set -uo pipefail

# ---------------------------------------------------------------------------
# The decision
# ---------------------------------------------------------------------------

# What state one worktree's `.git` entry is in.
#
# PURE: it is told what was found rather than looking. Every branch is one staged
# line in the self-test, including the two that need a git this machine does not
# have.
#
# @param 1 "dir" | "file" | "absent" -- what `<worktree>/.git` is
# @param 2 the `gitdir:` payload when it is a file, else ""
# @param 3 "yes" | "no" -- whether git can enumerate the tree as it stands
# @return echoes one of:
#           readable          nothing to do
#           absolute-gitdir   the pointer is absolute and this git cannot follow it
#           relative-broken   the pointer is relative and still does not resolve
#           not-a-worktree    no `.git` entry at all
#           plain-repo        `.git` is a directory: an ordinary clone, not linked
#           unreadable-other  git refuses for a reason this cannot name
worktree_pointer_state() {
    local kind="$1" payload="$2" enumerates="$3"

    if [ "$enumerates" = "yes" ]; then
        echo "readable"
        return 0
    fi

    case "$kind" in
        absent) echo "not-a-worktree"; return 0 ;;
        dir)    echo "plain-repo"; return 0 ;;
    esac

    # A `.git` FILE that git cannot follow. Which KIND matters: an absolute pointer
    # is this defect and is repairable from here, while a relative one that still
    # fails is something else -- a moved repository, a deleted admin directory --
    # and rewriting it would be guessing.
    case "$payload" in
        "")            echo "unreadable-other" ;;
        /*|[A-Za-z]:*) echo "absolute-gitdir" ;;
        *)             echo "relative-broken" ;;
    esac
}

# The relative path from $1 to $2, both absolute and normalised.
#
# Hand-rolled because the portable alternatives are not: `realpath --relative-to`
# is GNU-only and macOS ships neither it nor `--relative-base`, and this script has
# to run wherever the gate does. No `readlink -f` either, which resolves symlinks
# and would rewrite a pointer through whatever link the operator reached the tree
# by -- the two paths must stay in the operator's own spelling or the result is a
# pointer that is correct and unrecognisable.
#
# @param 1 from (a directory)
# @param 2 to
relative_path() {
    local from="$1" to="$2"
    local common="$from" up=""
    while [ "${to#"${common}/"}" = "$to" ] && [ "$to" != "$common" ]; do
        common="$(dirname "$common")"
        up="../${up}"
        [ "$common" != "/" ] || break
    done
    if [ "$to" = "$common" ]; then
        printf '%s\n' "${up%/}"
        return 0
    fi
    printf '%s\n' "${up}${to#"${common}/"}"
}

# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

# What `<worktree>/.git` is, and what it says.
# Sets `wt_kind` and `wt_payload`.
read_git_entry() {
    local root="$1"
    wt_kind="absent"
    wt_payload=""
    if [ -d "${root}/.git" ]; then
        wt_kind="dir"
    elif [ -f "${root}/.git" ]; then
        wt_kind="file"
        local line=""
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                gitdir:*)
                    wt_payload="${line#gitdir:}"
                    # Leading blanks only; a path may legitimately end in one.
                    while [ "${wt_payload# }" != "$wt_payload" ]; do wt_payload="${wt_payload# }"; done
                    break
                    ;;
            esac
        done < "${root}/.git"
    fi
}

# Can git enumerate this tree AS IT STANDS?
#
# `git ls-files` and a COUNT, never `rev-parse`: zero files is the state that
# matters and it is what every consumer in the gate actually meets, while
# `rev-parse` can answer for a repository whose index this git cannot read. And a
# count rather than a status, because zero is the answer to look for -- the same
# reason `git ls-files '*.cpp' | wc -l` is the check in the team guide rather than
# `rev-parse`.
git_enumerates() {
    local root="$1" n
    n="$( cd "$root" 2>/dev/null && git ls-files 2>/dev/null | grep -c . )" || n=0
    [ "${n:-0}" -gt 0 ]
}

# Translate a Windows path to this system's spelling, when that is meaningful.
#
# `wslpath` and nothing hand-rolled: a `D:` to `/mnt/d` mapping looks like two
# lines of `sed` and is wrong the moment a machine mounts its drives anywhere else,
# which `/etc/wsl.conf` lets it do. Where `wslpath` is absent the path is handed
# back unchanged and the caller checks whether it exists -- so a non-WSL host
# simply finds that it does not, and refuses, rather than acting on a translation
# nobody could verify.
#
# **ONLY WHEN THE PATH IS ACTUALLY A WINDOWS ONE, and this was a live defect rather
# than a precaution.** `wslpath -u` reads its argument as a WINDOWS path whatever it
# looks like, so a POSIX-absolute one is taken as drive-relative and resolved
# against the current drive: measured, `/tmp/tmp.XXXX/admin` came back as
# `/mnt/d/tmp/tmp.XXXX/admin`, a directory that does not exist. The repair then
# refused, naming a path nobody had written, for a pointer that was perfectly
# resolvable.
#
# It hid because the case this script was WRITTEN against is a real Windows path
# (`D:/repo/.git/worktrees/x`), where the translation is correct -- so every
# manual check passed and only a self-test case with a POSIX pointer reached it.
# That is the `/tmp` trap this repository already records for Windows-native tools,
# arriving through `wslpath`.
#
# A drive letter or a backslash is what says "Windows"; anything else is handed
# back untouched.
to_local_path() {
    local p="$1"
    case "$p" in
        [A-Za-z]:*|*\\*) ;;
        *) printf '%s\n' "$p"; return 0 ;;
    esac
    if command -v wslpath >/dev/null 2>&1; then
        local out=""
        out="$(wslpath -u "$p" 2>/dev/null)" || out=""
        if [ -n "$out" ]; then printf '%s\n' "$out"; return 0; fi
    fi
    printf '%s\n' "$p"
}

# Point a linked worktree and its admin directory at each other RELATIVELY.
#
# The whole repair, in one function, so the self-test can drive it against a real
# git worktree without having to fake a second git dialect -- which is not
# stageable on one host, there being only one spelling of an absolute path there.
# What IS stageable, and is the property that matters, is that a worktree wired
# this way survives being MOVED: relative pointers keep resolving because neither
# side names a root, and that is the same reason two gits with different roots both
# read them.
#
# @param 1 the worktree
# @param 2 its admin directory, in this system's spelling
write_relative_pointers() {
    local root="$1" admin="$2"
    printf 'gitdir: %s\n' "$(relative_path "$root" "$admin")" > "${root}/.git" || return 1
    printf '%s\n' "$(relative_path "$admin" "${root}/.git")" > "${admin}/gitdir" || return 1
}

# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------

repair_worktree() {
    local root="$1" apply="$2"
    local state admin adminLocal name backTo fwdTo

    read_git_entry "$root"
    local enumerates="no"
    git_enumerates "$root" && enumerates="yes"
    state="$(worktree_pointer_state "$wt_kind" "$wt_payload" "$enumerates")"

    case "$state" in
        readable)
            echo "worktree: ${root} is readable by this git ($( cd "$root" && git ls-files | grep -c . ) tracked file(s))"
            return 0
            ;;
        plain-repo)
            echo "worktree: ${root} has a .git DIRECTORY, so it is an ordinary clone rather than a"
            echo "worktree:   linked worktree -- and it still does not enumerate. That is a different"
            echo "worktree:   fault and this script will not touch it."
            return 1
            ;;
        not-a-worktree)
            echo "worktree: ${root} has no .git entry at all, so it is not a work tree."
            return 1
            ;;
        relative-broken)
            echo "worktree: ${root}/.git already holds a RELATIVE pointer (${wt_payload}) and git still"
            echo "worktree:   cannot follow it. That is a moved repository or a deleted admin directory,"
            echo "worktree:   not this defect, and rewriting the pointer would be guessing."
            return 1
            ;;
        unreadable-other)
            echo "worktree: ${root}/.git is a file with no gitdir: line. This cannot name the fault."
            return 1
            ;;
    esac

    # --- absolute-gitdir, the case this exists for -------------------------
    echo "worktree: ${root}/.git holds an ABSOLUTE pointer that this git cannot follow:"
    echo "worktree:   ${wt_payload}"
    echo "worktree:   A worktree created by the other git on this machine looks exactly like"
    echo "worktree:   this. Every git command here answers nothing, so the gate's formatter"
    echo "worktree:   list, its header census and its test walk are all empty."

    adminLocal="$(to_local_path "$wt_payload")"
    if [ ! -d "$adminLocal" ]; then
        echo "worktree: the admin directory it names does not exist here either, as"
        echo "worktree:   '${adminLocal}'. Without it the two relative pointers cannot be computed,"
        echo "worktree:   so this REFUSES rather than writing a path it cannot verify."
        return 1
    fi

    name="$(basename "$adminLocal")"
    # From the worktree to its admin directory, and back from the admin directory to
    # the worktree's `.git` FILE -- git checks the second against the first.
    backTo="$(relative_path "$root" "$adminLocal")"
    fwdTo="$(relative_path "$adminLocal" "${root}/.git")"

    echo "worktree: the repair, which every git resolves identically:"
    echo "worktree:   ${root}/.git            -> gitdir: ${backTo}"
    echo "worktree:   ${adminLocal}/gitdir -> ${fwdTo}"

    if [ "$apply" != "apply" ]; then
        echo "worktree: NOT APPLIED. Re-run with --apply, or from the repository root:"
        echo "worktree:   bash scripts/repair-worktree-pointers.sh --apply"
        return 1
    fi

    write_relative_pointers "$root" "$adminLocal" || return 1

    # ASSERTED, not assumed. Writing the files is not the property; git reading the
    # tree is, and a repair that reported success on a tree still answering nothing
    # would be this ticket's own failure shape inside its fix.
    if git_enumerates "$root"; then
        echo "worktree: repaired -- git now enumerates $( cd "$root" && git ls-files | grep -c . ) file(s) here (worktree '${name}')"
        return 0
    fi
    echo "worktree: the pointers were rewritten and git STILL enumerates nothing. Something"
    echo "worktree:   else is wrong; the two files now read as shown above."
    return 1
}

# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

repair_self_test() {
    local ran=0 failed=0

    # An inherited `GIT_DIR`/`GIT_WORK_TREE` would make every `git init` below operate
    # on the REAL repository instead of on the fixture. Measured on a throwaway repo,
    # git 2.43.0: with that pair exported a fixture's `git init .` creates no `.git` of
    # its own at all, and the fixture's `git config user.email ...` lands in the SHARED
    # config -- so it is not one key leaking, it is every later git call in the fixture
    # reaching the real admin directory. `extensions.worktreeConfig = true`, which this
    # repository carries, does NOT contain it; that was checked rather than assumed.
    #
    # A lane was gating with exactly that pair exported while this was written, to work
    # around the very defect this script repairs. The header above has described the
    # hazard in prose since the file was created, and a comment is not a guard.
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

    _rw_expect() {
        ran=$(( ran + 1 ))
        if [ "$2" != "$3" ]; then
            echo "REPAIR SELF-TEST FAILED: $1: expected '$2', got '$3'" >&2
            failed=$(( failed + 1 ))
        fi
    }

    # --- the state machine, over staged facts ----------------------------
    _rw_expect "a tree git can enumerate needs nothing" \
        "readable" "$(worktree_pointer_state file '../../repo/.git/worktrees/x' yes)"
    # Readable wins even for a shape this would otherwise refuse: the question is
    # whether git can read the tree, and a `.git` directory that enumerates is an
    # ordinary clone doing its job.
    _rw_expect "and that is decided before the shape is looked at" \
        "readable" "$(worktree_pointer_state dir '' yes)"
    _rw_expect "a Windows-created worktree read from WSL is the repairable case" \
        "absolute-gitdir" "$(worktree_pointer_state file 'D:/repo/.git/worktrees/x' no)"
    # A POSIX-absolute pointer is the same defect from the other direction -- a
    # WSL-created worktree read from Windows git -- and must not be classified by
    # the drive letter alone.
    _rw_expect "so is a POSIX-absolute one, which has no drive letter to spot" \
        "absolute-gitdir" "$(worktree_pointer_state file '/mnt/d/repo/.git/worktrees/x' no)"
    _rw_expect "a relative pointer that still fails is NOT this defect" \
        "relative-broken" "$(worktree_pointer_state file '../../repo/.git/worktrees/x' no)"
    _rw_expect "a missing .git is its own answer" \
        "not-a-worktree" "$(worktree_pointer_state absent '' no)"
    _rw_expect "a plain clone that does not enumerate is refused, not rewritten" \
        "plain-repo" "$(worktree_pointer_state dir '' no)"
    _rw_expect "a .git file with no gitdir: line names its own ignorance" \
        "unreadable-other" "$(worktree_pointer_state file '' no)"

    # Six outcomes and they must be six: rows that answer alike would satisfy every
    # assertion above while the classifier decided one thing.
    local distinct
    distinct="$( { worktree_pointer_state file '../x' yes
                   worktree_pointer_state file 'D:/x' no
                   worktree_pointer_state file '../x' no
                   worktree_pointer_state absent '' no
                   worktree_pointer_state dir '' no
                   worktree_pointer_state file '' no; } | sort -u | grep -c . )"
    _rw_expect "the classifier has six distinct answers" "6" "$distinct"

    # --- the relative-path arithmetic ------------------------------------
    #
    # The two real shapes, and they are not symmetrical: the worktree reaches the
    # admin directory by going up TWO levels, and the admin directory reaches back
    # by going up FOUR. Getting either wrong writes a pointer that resolves to a
    # path which does not exist, so both are pinned with the literals this machine
    # actually uses.
    _rw_expect "worktree -> admin directory" \
        "../../fastcached/.git/worktrees/gate" \
        "$(relative_path /mnt/d/fastcached-worktrees/gate /mnt/d/fastcached/.git/worktrees/gate)"
    _rw_expect "admin directory -> worktree/.git" \
        "../../../../fastcached-worktrees/gate/.git" \
        "$(relative_path /mnt/d/fastcached/.git/worktrees/gate /mnt/d/fastcached-worktrees/gate/.git)"
    _rw_expect "a target inside the source is a plain descent" \
        "a/b" "$(relative_path /x /x/a/b)"

    # --- the path dialect -------------------------------------------------
    #
    # A POSIX pointer must survive untouched. `wslpath -u` reads ANY argument as a
    # Windows path, so an unguarded call resolved `/tmp/x` against the current
    # drive and answered `/mnt/d/tmp/x` -- and the repair then refused, naming a
    # directory nobody had written, for a pointer that was perfectly good. Measured
    # here; it hid because the case this was written against is a real `D:` path,
    # where the translation is right.
    _rw_expect "a POSIX pointer is not run through a Windows translator" \
        "/tmp/somewhere/admin" "$(to_local_path /tmp/somewhere/admin)"
    _rw_expect "and neither is a relative one" \
        "../../repo/.git/worktrees/x" "$(to_local_path ../../repo/.git/worktrees/x)"

    # --- and a REAL repair, on a real linked worktree ---------------------
    #
    # WHAT CAN AND CANNOT BE STAGED HERE, because the first version of this staged
    # the wrong thing and failed honestly. The defect is an absolute pointer in a
    # dialect the READING git cannot resolve, and one host has only one spelling of
    # an absolute path -- so "absolute and unfollowable" cannot be built on Linux
    # without also making the path not exist, which is a DIFFERENT state this script
    # already refuses by name.
    #
    # What is stageable is the property the repair actually rests on: a worktree
    # wired with relative pointers keeps working when the whole arrangement MOVES,
    # because neither side names a root. That is the same reason two gits with
    # different roots both read it, and it is checked against real git rather than
    # argued.
    if ! command -v git >/dev/null 2>&1; then
        echo "repair-worktree-pointers --self-test: ${ran} checks ran, ${failed} failed -- SKIPPED: no git"
        [ "$failed" -eq 0 ]
        return $?
    fi
    local scratch repo wt
    scratch="$(mktemp -d)"
    repo="${scratch}/repo"
    wt="${scratch}/trees/lane"
    mkdir -p "$repo" "${scratch}/trees"
    (
        cd "$repo" || exit 1
        git init -q .
        git config user.email t@example.invalid
        git config user.name t
        echo hello > a.txt
        git add a.txt
        git commit -q -m first
        git worktree add -q "$wt" -b lane
    ) >/dev/null 2>&1

    if [ ! -e "${wt}/.git" ]; then
        rm -rf "$scratch"
        echo "repair-worktree-pointers --self-test: ${ran} checks ran, ${failed} failed -- SKIPPED: git could not stage a linked worktree"
        [ "$failed" -eq 0 ]
        return $?
    fi

    # A positive control on the FIXTURE: git's own default really is absolute, so
    # the rewrite below is a change rather than a no-op. If git ever started writing
    # relative pointers this whole section would be vacuous and nothing else would
    # say so.
    # `case` at STATEMENT level, never inside `$( )`. bash 3.2 -- which macOS ships,
    # and which every default-set script must parse under -- counts parentheses
    # naively inside a command substitution, so the `)` that closes a case PATTERN is
    # read as closing the `$(`.
    #
    # Measured on `macOS-clang-release` (#1224): the substitution truncated at
    # `gitdir:\ /*`, the expansion returned the literal remainder of this file's own
    # source -- ` echo yes ;; *) echo no ;; esac)` -- and `_rw_expect` compared against
    # THAT. So it reported a wrong ANSWER where the truth was that the instrument had
    # not run, sending a reader to git's pointer format rather than to a parse error
    # four lines up. It refused rather than passing, which is the one mercy.
    #
    # Same family as the rulebook's heredoc-inside-`$( )`, and NOT on the bash-3.2
    # scan's token list (`mapfile`, `declare -A`, `${var^^}`, `local -n`) -- a
    # multi-line-equivalent construct inside a substitution passes that scan and dies
    # on the host.
    local wtPointer adminPointer verdict
    wtPointer="$(cat "${wt}/.git")"
    case "$wtPointer" in gitdir:\ /*) verdict=yes ;; *) verdict=no ;; esac
    _rw_expect "git wrote an ABSOLUTE pointer, which is what makes this necessary" \
        "yes" "$verdict"

    write_relative_pointers "$wt" "${repo}/.git/worktrees/lane"

    wtPointer="$(cat "${wt}/.git")"
    case "$wtPointer" in
        gitdir:\ /*|gitdir:\ [A-Za-z]:*) verdict=no ;;
        gitdir:*)                        verdict=yes ;;
        *)                               verdict=no ;;
    esac
    _rw_expect "the worktree pointer is relative afterwards" "yes" "$verdict"

    adminPointer="$(cat "${repo}/.git/worktrees/lane/gitdir")"
    case "$adminPointer" in
        /*|[A-Za-z]:*) verdict=no ;;
        ?*)            verdict=yes ;;
        *)             verdict=no ;;
    esac
    _rw_expect "and so is the admin pointer" "yes" "$verdict"
    _rw_expect "git still reads the worktree" \
        "yes" "$( ( cd "$wt" && git ls-files 2>/dev/null | grep -c . ) | { read -r n; [ "${n:-0}" -gt 0 ] && echo yes || echo no; } )"

    # THE ONE THAT MATTERS: move the whole arrangement and read it again. Absolute
    # pointers would now name a directory that is not there, which is the failure
    # the real machine reaches by a different route.
    mv "$scratch" "${scratch}-moved"
    scratch="${scratch}-moved"
    repo="${scratch}/repo"
    wt="${scratch}/trees/lane"
    _rw_expect "and still reads it after the whole arrangement MOVES" \
        "yes" "$( ( cd "$wt" && git ls-files 2>/dev/null | grep -c . ) | { read -r n; [ "${n:-0}" -gt 0 ] && echo yes || echo no; } )"

    # A tree that is already fine is reported readable and LEFT ALONE -- the
    # accepting direction, which a guard nobody has watched accept is not known to
    # have. It also proves the classifier's `readable` arm is reachable from a real
    # tree rather than only from a staged row.
    local before
    before="$(cat "${wt}/.git")"
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "a healthy worktree is accepted" "0" "$?"
    _rw_expect "and is not rewritten" "$before" "$(cat "${wt}/.git")"

    # The refusal that the moved tree makes reachable for real: point it back at an
    # absolute path that does not exist, which is what a repair cannot compute from.
    printf 'gitdir: %s\n' "/nonexistent-$$/repo/.git/worktrees/lane" > "${wt}/.git"
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "an absolute pointer to nothing is REFUSED rather than guessed at" "1" "$?"

    # AND THE CASE THAT CATCHES A REPAIR CLAIMING SUCCESS IT DID NOT ACHIEVE. The
    # rows above cannot: none of them reaches the write with a tree that stays
    # unreadable, so replacing the post-repair `git_enumerates` check with `true`
    # left all twenty green -- measured, and it is this ticket's own failure shape
    # sitting inside its fix.
    #
    # Staged by naming an admin directory that EXISTS and is not a git admin
    # directory at all: the existence guard passes, the pointers are written, and
    # git still reads nothing. Writing the files is not the property; git reading
    # the result is.
    mkdir -p "${scratch}/decoy-admin"
    printf 'gitdir: %s\n' "${scratch}/decoy-admin" > "${wt}/.git"
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "a repair that leaves the tree unreadable REFUSES, having written the files" \
        "1" "$?"

    rm -rf "$scratch"
    echo "repair-worktree-pointers --self-test: ${ran} checks ran, ${failed} failed"
    [ "$failed" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Entry
# ---------------------------------------------------------------------------

wt_kind=""
wt_payload=""
repair_root=""
repair_apply="report"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --self-test) repair_self_test; exit $? ;;
        --apply)     repair_apply="apply"; shift ;;
        -*)          echo "usage: $0 [--apply] [<worktree>] | --self-test" >&2; exit 2 ;;
        *)           repair_root="$1"; shift ;;
    esac
done

if [ -z "$repair_root" ]; then
    repair_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi

repair_worktree "$repair_root" "$repair_apply"
