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
# Rewrites both pointers RELATIVE, which both gits resolve identically -- neither a
# drive letter nor a mount point appears in them -- PROVIDED the worktree and the
# repository sit on the same Windows root:
#
#     <worktree>/.git                      gitdir: ../../repo/.git/worktrees/<name>
#     <repo>/.git/worktrees/<name>/gitdir  ../../../../worktrees/<name>/.git
#
# **That proviso is not a detail; an earlier version of this header said "every git"
# without it, and it was false.** WSL mounts every drive under one root (`/mnt/c`,
# `/mnt/d`), so a relative path can climb out of one drive and into another -- and
# Windows cannot: there is no `..` above `C:\`. A worktree on C: of a repository on D:
# got `gitdir: ../../<...>/d/<repo>/...`, which WSL read and Windows git refused as "not
# a git repository", while this script printed "repaired" and exited 0, having checked
# only with the git that ran it. So the two Windows roots -- a drive letter, or a UNC
# share such as WSL's own `\\wsl.localhost\<distro>` -- are compared BEFORE anything is
# written, and a mismatch is REFUSED by name: no relative pointer can serve both gits.
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
# RUN IT UNDER THE GIT THAT CANNOT READ THE WORKTREE, which is the whole point
# and is easy to get backwards. The absolute `D:/` pointer is unreadable to WSL's
# git and perfectly readable to Windows git, so running this from Git Bash on a
# worktree WSL cannot open prints
#
#     worktree: <path> is readable by this git (N tracked file(s))
#
# and repairs nothing. That sentence is TRUE -- it says *this* git -- and it reads
# as success, so the next thing that runs under WSL fails for a reason the check
# just appeared to rule out. Measured: a session lost a `ctest` run to exactly
# that, diagnosing a registration defect that did not exist. A claim about a tool
# is checked against THAT tool; here the tool is the git that is failing.
#
# Usage:
#   scripts/repair-worktree-pointers.sh                 diagnose this worktree
#   scripts/repair-worktree-pointers.sh --apply         and repair it
#   scripts/repair-worktree-pointers.sh --self-test
#
#   wsl -e bash scripts/repair-worktree-pointers.sh --apply     from Windows, when
#                                                               WSL is the git that
#                                                               cannot read it
#
# Exit, one code per ANSWER, so a caller can tell "run it with --apply" from "this
# script cannot help" without parsing prose:
#   0  the worktree is readable (already, or after `--apply`)
#   1  it is not, and it IS repairable: report-only mode did not apply the repair --
#      re-run with `--apply`. Never 0: the tree is still broken, and a status saying
#      otherwise would be read as one that had been repaired.
#   2  usage, or a path that cannot be entered
#   3  it is not readable and this script CANNOT repair it, in either mode, refused by
#      name: no `.git`, a plain clone, a relative pointer that still fails, a fault it
#      cannot name, an admin directory that does not exist here, pointers it cannot
#      compute, or a repair that left the tree unreadable
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
        # A UNC path (`\\server\share\...`, either separator) is as absolute as a drive
        # letter; the same two-separator shape `windows_root_of` reads its share from.
        /*|[A-Za-z]:[/\\]*|[/\\][/\\]?*) echo "absolute-gitdir" ;;
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
# **Both must be ABSOLUTE, and anything else is REFUSED rather than walked.** The walk
# climbs with `dirname` until it reaches a common prefix or `/`, and `dirname .` is
# `.`: a relative input never reaches `/`, so `--apply .` spun here forever (killed by
# hand after 21 minutes). A drive-letter path (`D:/x`) is refused by the same rule,
# since it is no more comparable with a POSIX one than `.` is.
#
# @param 1 from (a directory)
# @param 2 to
# @return 0 with the path printed; 2, printing nothing, when either input is not absolute.
relative_path() {
    local from="$1" to="$2"
    case "$from" in /*) ;; *) echo "relative_path: '${from}' is not an absolute path" >&2; return 2 ;; esac
    case "$to" in /*) ;; *) echo "relative_path: '${to}' is not an absolute path" >&2; return 2 ;; esac
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

# This system's spelling of a path, in WINDOWS form -- `D:\x` from `/mnt/d/x` under WSL,
# or from `/d/x` under Git Bash.
#
# Acquisition only; `windows_side_of` decides. `wslpath -w` and `cygpath -w` are each
# host's own answer, for `to_local_path`'s reason: a mount table is not ours to model.
#
# **"No translator" and "the translator FAILED" are different answers, and the status
# keeps them apart.** They were one empty string, which the verdict read as "no Windows
# side at all" -- so a `wslpath -w` that failed on a C:-of-D: worktree let the repair
# write `../d/...`, print "repaired" and exit 0: the original defect, fail-open
# (reproduced by a reviewer on the real tree).
# @param 1 A path in this system's spelling.
# @return 0 with the Windows spelling printed; 1, printing nothing, when this host has
#         no translator (a plain POSIX host); 2, printing nothing, when a translator is
#         present and failed or answered nothing.
to_windows_path() {
    local p="$1" out=""
    if command -v wslpath >/dev/null 2>&1; then
        out="$(wslpath -w "$p" 2>/dev/null)" || return 2
    elif command -v cygpath >/dev/null 2>&1; then
        out="$(cygpath -w "$p" 2>/dev/null)" || return 2
    else
        return 1
    fi
    [ -n "$out" ] || return 2
    printf '%s\n' "$out"
}

# The Windows ROOT a Windows path lives on: a drive (`D:`) or a UNC share
# (`//server/share`, WSL's own being `//wsl.localhost/<distro>`), in one case-folded
# spelling so two answers compare as strings. PURE.
#
# Nothing above the root is reachable by a relative path on Windows, which is the
# whole reason this is asked (see the header).
# @param 1 A Windows path, either separator, or "".
# @return echoes the root, or "" when the input names none (empty, drive-relative
#         `C:x`, or not a Windows path at all).
windows_root_of() {
    local w="$1" u rest server share
    case "$w" in
        [A-Za-z]:[/\\]*)
            printf '%s:\n' "$(printf '%s' "${w%%:*}" | tr '[:lower:]' '[:upper:]')"
            return 0 ;;
        [/\\][/\\]?*)
            u="$(printf '%s' "$w" | tr '\\' '/')"
            rest="${u#//}"
            server="${rest%%/*}"
            share="${rest#*/}"
            share="${share%%/*}"
            if [ -z "$server" ] || [ -z "$share" ] || [ "$share" = "$rest" ]; then
                echo ""
                return 0
            fi
            printf '//%s/%s\n' "$(printf '%s' "$server" | tr '[:upper:]' '[:lower:]')" \
                "$(printf '%s' "$share" | tr '[:upper:]' '[:lower:]')"
            return 0 ;;
    esac
    echo ""
}

# Which Windows side one path is on, as one token for `windows_roots_verdict`: its
# root (`D:`, `//server/share`), `-` when this host has no translator at all, or `?`
# when a translator is present and failed -- or answered with no root to read.
# Four states, never three: `-` is a fact about the HOST, `?` a failure to learn one.
# @param 1 A path in this system's spelling.
# @return echoes the token.
windows_side_of() {
    local w rc root
    w="$(to_windows_path "$1")"
    rc=$?
    case "$rc" in
        0)  root="$(windows_root_of "$w")"
            if [ -n "$root" ]; then echo "$root"; else echo "?"; fi ;;
        1)  echo "-" ;;
        *)  echo "?" ;;
    esac
}

# Whether a relative pointer between two paths can serve BOTH gits. PURE.
#
# @param 1 the worktree's side, from `windows_side_of`
# @param 2 the admin directory's
# @return echoes one of:
#           same      one root, or no translator on either side (a plain POSIX host,
#                     where no Windows git will ever read the pointer)
#           differ    two roots: a relative path cannot cross them on Windows
#           unknown   anything else -- a translator that failed on either side, or one
#                     side translated and the other not -- refused, since a pointer this
#                     cannot vouch for is the defect it exists to prevent
windows_roots_verdict() {
    local a="$1" b="$2"
    if [ "$a" = "-" ] && [ "$b" = "-" ]; then
        echo "same"
    elif [ "$a" = "-" ] || [ "$b" = "-" ] || [ "$a" = "?" ] || [ "$b" = "?" ] \
        || [ -z "$a" ] || [ -z "$b" ]; then
        echo "unknown"
    elif [ "$a" = "$b" ]; then
        echo "same"
    else
        echo "differ"
    fi
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
    # A drive letter, a backslash anywhere, or a UNC path in EITHER spelling: Git for
    # Windows writes a WSL-native repository's pointer as the forward-slash
    # `//wsl.localhost/<distro>/...`, which passed through untranslated and was refused as
    # an admin directory that "does not exist here" -- although `wslpath -u` answers it.
    # The same two-separator shape `worktree_pointer_state` and `windows_root_of` read.
    case "$p" in
        [A-Za-z]:[/\\]*|[/\\][/\\]?*|*\\*) ;;
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
    local root="$1" admin="$2" back fwd
    # Both computed BEFORE either file is written, so a refusal leaves both untouched.
    back="$(relative_path "$root" "$admin")" || return 1
    fwd="$(relative_path "$admin" "${root}/.git")" || return 1
    printf 'gitdir: %s\n' "$back" > "${root}/.git" || return 1
    printf '%s\n' "$fwd" > "${admin}/gitdir" || return 1
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
            return 3
            ;;
        not-a-worktree)
            echo "worktree: ${root} has no .git entry at all, so it is not a work tree."
            return 3
            ;;
        relative-broken)
            echo "worktree: ${root}/.git already holds a RELATIVE pointer (${wt_payload}) and git still"
            echo "worktree:   cannot follow it. That is a moved repository or a deleted admin directory,"
            echo "worktree:   not this defect, and rewriting the pointer would be guessing."
            return 3
            ;;
        unreadable-other)
            echo "worktree: ${root}/.git is a file with no gitdir: line. This cannot name the fault."
            return 3
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
        return 3
    fi

    # One Windows root, or no relative pointer can serve both gits (see the header).
    local rootWin adminWin
    rootWin="$(windows_side_of "$root")"
    adminWin="$(windows_side_of "$adminLocal")"
    case "$(windows_roots_verdict "$rootWin" "$adminWin")" in
        same) ;;
        differ)
            echo "worktree: the worktree is on ${rootWin} and its repository's admin directory on"
            echo "worktree:   ${adminWin}. A relative pointer between two Windows roots resolves under"
            echo "worktree:   WSL, whose mounts share one root, and NOT under Windows git, which"
            echo "worktree:   cannot climb above a drive -- so no rewrite serves both, and this REFUSES."
            echo "worktree:   Put the worktree on the repository's drive: git worktree add <path on ${adminWin}>."
            return 3 ;;
        *)
            echo "worktree: could not tell which Windows root '${root}' (${rootWin}) and"
            echo "worktree:   '${adminLocal}' (${adminWin}) are on -- '?' is a translator that failed,"
            echo "worktree:   '-' a side with none -- so a relative pointer between them cannot be"
            echo "worktree:   vouched for; REFUSED."
            return 3 ;;
    esac

    name="$(basename "$adminLocal")"
    # From the worktree to its admin directory, and back from the admin directory to
    # the worktree's `.git` FILE -- git checks the second against the first.
    if ! backTo="$(relative_path "$root" "$adminLocal")" || ! fwdTo="$(relative_path "$adminLocal" "${root}/.git")"; then
        echo "worktree: '${root}' and '${adminLocal}' are not both absolute paths in this"
        echo "worktree:   system's spelling, so the relative pointers cannot be computed; REFUSED."
        return 3
    fi

    echo "worktree: the repair, which every git resolves identically:"
    echo "worktree:   ${root}/.git            -> gitdir: ${backTo}"
    echo "worktree:   ${adminLocal}/gitdir -> ${fwdTo}"

    if [ "$apply" != "apply" ]; then
        echo "worktree: NOT APPLIED. Re-run with --apply, or from the repository root:"
        echo "worktree:   bash scripts/repair-worktree-pointers.sh --apply"
        return 1
    fi

    write_relative_pointers "$root" "$adminLocal" || return 3

    # ASSERTED, not assumed. Writing the files is not the property; git reading the
    # tree is, and a repair that reported success on a tree still answering nothing
    # would be this ticket's own failure shape inside its fix.
    if git_enumerates "$root"; then
        echo "worktree: repaired -- git now enumerates $( cd "$root" && git ls-files | grep -c . ) file(s) here (worktree '${name}')"
        return 0
    fi
    echo "worktree: the pointers were rewritten and git STILL enumerates nothing. Something"
    echo "worktree:   else is wrong; the two files now read as shown above."
    return 3
}

# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

# This script, for the self-test's command-line rows; captured before anything `cd`s.
repair_self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

# How many checks a FULL `repair_self_test` runs -- both halves, no skip arm taken.
# Asserted, and the pass is a line of its own, because the count line alone could not
# tell a run that judged everything from one that stopped early: a `return 0` planted at
# the top of the function exited 0 with no output, and ctest said Passed. Change it in
# the same edit that adds or removes a row. The skip arms return 77 before the
# assertion and print no pass line: a skip stays a skip.
RepairSelfTestChecks=47

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
    # `C:repo` is DRIVE-RELATIVE -- relative to C:'s current directory -- and no more an
    # absolute pointer than `../repo` is; a bare `[A-Za-z]:*` pattern took it for one.
    _rw_expect "a drive-relative pointer is relative, not absolute" \
        "relative-broken" "$(worktree_pointer_state file 'C:repo/.git/worktrees/x' no)"
    _rw_expect "a UNC pointer is absolute, in backslash spelling" \
        "absolute-gitdir" "$(worktree_pointer_state file '\\server\share\repo\.git\worktrees\x' no)"
    # This spelling is classified by the `/*` alternative, which precedes the UNC one in
    # the same arm -- so the row pins the ANSWER for the spelling Git for Windows writes
    # for a WSL-native repository, not the new alternative; the backslash row above is
    # the one that needs it.
    _rw_expect "and in forward-slash spelling" \
        "absolute-gitdir" "$(worktree_pointer_state file '//server/share/repo/.git/worktrees/x' no)"
    _rw_expect "while a drive letter with a separator IS absolute, either separator" \
        "absolute-gitdir absolute-gitdir" \
        "$(worktree_pointer_state file 'C:/repo/.git/worktrees/x' no) $(worktree_pointer_state file 'C:\repo\.git\worktrees\x' no)"
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
    # A relative input is REFUSED, never walked: `dirname .` is `.`, and the walk spun
    # forever on `--apply .`. Each row reads the status, so a hang shows as the ctest
    # TIMEOUT and a silent wrong answer as a mismatch.
    _rw_expect "a source of '.' is refused, not walked" \
        "status 2, output []" "$(out="$(relative_path . /x/a/b 2>/dev/null)"; echo "status $?, output [${out}]")"
    _rw_expect "so is a relative source" \
        "status 2, output []" "$(out="$(relative_path trees/lane /x/a/b 2>/dev/null)"; echo "status $?, output [${out}]")"
    _rw_expect "and a relative target" \
        "status 2, output []" "$(out="$(relative_path /x a/b 2>/dev/null)"; echo "status $?, output [${out}]")"
    _rw_expect "and a drive-letter one, which no POSIX path is comparable with" \
        "status 2, output []" "$(out="$(relative_path /x D:/x/a 2>/dev/null)"; echo "status $?, output [${out}]")"

    # --- one Windows root, or no relative pointer serves both gits ------------
    _rw_expect "a drive's root is its letter, case-folded, either separator" \
        "D: D: C:" "$(windows_root_of 'D:\repo\x') $(windows_root_of 'd:/repo') $(windows_root_of 'C:\')"
    _rw_expect "a UNC path's root is its server and share, case-folded" \
        "//wsl.localhost/ubuntu //server/share" \
        "$(windows_root_of '\\wsl.localhost\Ubuntu\home\u') $(windows_root_of '//Server/Share/x')"
    _rw_expect "a drive-relative, empty or POSIX path names no root" \
        "[] [] []" "[$(windows_root_of 'C:repo')] [$(windows_root_of '')] [$(windows_root_of '/mnt/d/x')]"
    _rw_expect "one root is the same, two differ, no translator on either side is a POSIX host" \
        "same differ same" \
        "$(windows_roots_verdict D: D:) $(windows_roots_verdict C: D:) $(windows_roots_verdict - -)"
    _rw_expect "and a FAILED translator, on either side or both, or one side untranslated, is unknown" \
        "unknown unknown unknown unknown" \
        "$(windows_roots_verdict D: '?') $(windows_roots_verdict '?' '?') $(windows_roots_verdict D: -) $(windows_roots_verdict - '?')"

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
    _rw_expect "nor a drive-relative one, which names no root to translate" \
        "C:repo/.git/worktrees/x" "$(to_local_path C:repo/.git/worktrees/x)"

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
    # 77, never 0 (#1231). `[ "$failed" -eq 0 ]; return $?` reports **Passed** for a
    # run that never staged the real repair -- the SUCCEED-standing-in-for-a-skip
    # shape, arriving exactly on the hosts where coverage is thinnest. A failure
    # already recorded OUTRANKS the skip: a run that skipped the git half and failed
    # the pure half is a FAILURE, and returning 77 there would hide it behind a state
    # ctest prints as green-ish.
    if ! command -v git >/dev/null 2>&1; then
        echo "repair-worktree-pointers --self-test: ${ran} checks ran, ${failed} failed -- SKIPPED: no git"
        [ "$failed" -eq 0 ] || return 1
        return 77
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
        [ "$failed" -eq 0 ] || return 1
        return 77
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
    # A drive letter is absolute too: Git for Windows writes `gitdir: C:/...`, so a
    # POSIX-only pattern failed this control on Git Bash while git had done exactly
    # what the control asserts.
    case "$wtPointer" in gitdir:\ /*|gitdir:\ [A-Za-z]:[/\\]*) verdict=yes ;; *) verdict=no ;; esac
    _rw_expect "git wrote an ABSOLUTE pointer, which is what makes this necessary" \
        "yes" "$verdict"

    write_relative_pointers "$wt" "${repo}/.git/worktrees/lane"

    wtPointer="$(cat "${wt}/.git")"
    case "$wtPointer" in
        gitdir:\ /*|gitdir:\ [A-Za-z]:[/\\]*) verdict=no ;;
        gitdir:*)                        verdict=yes ;;
        *)                               verdict=no ;;
    esac
    _rw_expect "the worktree pointer is relative afterwards" "yes" "$verdict"

    adminPointer="$(cat "${repo}/.git/worktrees/lane/gitdir")"
    case "$adminPointer" in
        /*|[A-Za-z]:[/\\]*) verdict=no ;;
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
    # Report-only first: a tree this cannot repair must not answer like one it could.
    repair_worktree "$wt" report >/dev/null 2>&1
    _rw_expect "report-only on a tree it cannot repair answers 3, not the repairable 1" "3" "$?"
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "an absolute pointer to nothing is REFUSED rather than guessed at" "3" "$?"

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
    # THE CROSS-ROOT REFUSAL, through the real repair: the translator is staged to put
    # the worktree on C: and the admin directory on D:, which one host cannot build for
    # real, and the repair must refuse BEFORE writing either pointer.
    local savedTranslator
    savedTranslator="$(declare -f to_windows_path)"
    to_windows_path() {
        case "$1" in
            "$wt") printf 'C:\\trees\\lane\n' ;;
            *)     printf 'D:\\repo\\.git\\worktrees\\lane\n' ;;
        esac
    }
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "a worktree on another Windows root than its repository is REFUSED, unwritten" \
        "3 gitdir: ${scratch}/decoy-admin" "$? $(cat "${wt}/.git")"

    # The repair's `unknown` arm, driven: one side translates and the other answers
    # nothing, which is no root to compare. Nothing else reaches that arm through the
    # real repair, so without this a `same|unknown) ;;` would pass the whole test.
    printf 'gitdir: %s\n' "${scratch}/decoy-admin" > "${wt}/.git"
    to_windows_path() {
        case "$1" in
            "$wt") printf 'C:\\trees\\lane\n' ;;
            *)     printf '' ;;
        esac
    }
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "a side the translator answers nothing for is unknown, REFUSED, unwritten" \
        "3 gitdir: ${scratch}/decoy-admin" "$? $(cat "${wt}/.git")"
    eval "$savedTranslator"
    # Re-planted, so the row below reads its own fixture rather than whatever a failed
    # row above left in the file.
    printf 'gitdir: %s\n' "${scratch}/decoy-admin" > "${wt}/.git"

    # A translator that is PRESENT and FAILS, staged on PATH so the acquisition itself is
    # what is driven: it once answered "" for this, read as "no Windows side", and the
    # repair wrote across two drives and reported success.
    mkdir -p "${scratch}/failbin"
    printf '#!/usr/bin/env bash\nexit 1\n' > "${scratch}/failbin/wslpath"
    chmod +x "${scratch}/failbin/wslpath"
    PATH="${scratch}/failbin:${PATH}" repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "a translator that fails is refused as unknown, never read as no Windows side" \
        "3 gitdir: ${scratch}/decoy-admin" "$? $(cat "${wt}/.git")"
    printf 'gitdir: %s\n' "${scratch}/decoy-admin" > "${wt}/.git"

    # Report-only over a tree whose pointer it CAN compute: 1, re-run with --apply, and
    # nothing written.
    repair_worktree "$wt" report >/dev/null 2>&1
    _rw_expect "report-only on a repairable tree answers 1 and applies nothing" \
        "1 gitdir: ${scratch}/decoy-admin" "$? $(cat "${wt}/.git")"
    repair_worktree "$wt" apply >/dev/null 2>&1
    _rw_expect "a repair that leaves the tree unreadable REFUSES, having written the files" \
        "3" "$?"

    # THE COMMAND LINE, as an operator types it, over the same unreadable tree -- the
    # state that reaches `relative_path`. `--apply .` and `--apply lane` from its parent
    # used to hand the relative spelling straight through and spin forever; the entry
    # now normalises it, so both reach the repair and refuse as above.
    printf 'gitdir: %s\n' "${scratch}/decoy-admin" > "${wt}/.git"
    ( cd "$wt" && bash "$repair_self" --apply . ) >/dev/null 2>&1
    _rw_expect "'--apply .' reaches the repair and refuses, never spinning" "3" "$?"
    # And it got there through the NORMALISED path, not by relative_path refusing the
    # dot: only a repair that computed the pointers wrote a relative one. (Statement-level
    # `case`, for bash 3.2's reason above.)
    wtPointer="$(cat "${wt}/.git")"
    case "$wtPointer" in
        gitdir:\ /*|gitdir:\ [A-Za-z]:[/\\]*) verdict=no ;;
        gitdir:\ ?*)                     verdict=yes ;;
        *)                               verdict=no ;;
    esac
    _rw_expect "and computed its pointers from the normalised path" "yes" "$verdict"
    printf 'gitdir: %s\n' "${scratch}/decoy-admin" > "${wt}/.git"
    ( cd "${scratch}/trees" && bash "$repair_self" --apply lane ) >/dev/null 2>&1
    _rw_expect "and so does a relative path to the tree" "3" "$?"
    ( cd "$scratch" && bash "$repair_self" --apply "no-such-tree-$$" ) >/dev/null 2>&1
    _rw_expect "a path that cannot be entered is refused by name" "2" "$?"

    # A Windows-created worktree of a WSL-NATIVE repository: Git for Windows writes the
    # pointer as the forward-slash UNC `//wsl.localhost/<distro>/...`. That is REPAIRABLE
    # -- `wslpath -u` answers it, and hand-written relative pointers are read by both gits
    # -- and was refused. Staged with a `wslpath` that translates one share both ways,
    # over the REAL admin directory, so the repair runs end to end and real git must read
    # the result.
    #
    # The shape is Git for Windows's; the SERVER is not. On Windows `//wsl.localhost/<any>`
    # is a live redirector -- measured under Git Bash, an unknown distro answered `-d` as
    # EXISTING in 48 ms, and with such a pointer the whole self-test went from 3 s to 21 s
    # -- and ran without end once the fix was neutered. `//localhost/<a share nobody has>` is the same
    # two-separator UNC shape, refused locally in 30 ms (measured), and names no path on a
    # POSIX host either -- so the pointer is unreadable before the repair wherever this runs.
    mkdir -p "${scratch}/uncbin"
    cat > "${scratch}/uncbin/wslpath" <<'STUB'
#!/usr/bin/env bash
# A staged wslpath for one share, //localhost/fc-test-share: -u drops it, -w adds it.
p="$2"
case "$1" in
    -u) p="$(printf '%s' "$p" | tr '\\' '/')"
        case "$p" in
            //localhost/fc-test-share/*) printf '/%s\n' "${p#//localhost/fc-test-share/}"; exit 0 ;;
        esac
        exit 1 ;;
    -w) printf '\\\\localhost\\fc-test-share%s\n' "$(printf '%s' "$p" | tr '/' '\\')"; exit 0 ;;
esac
exit 1
STUB
    chmod +x "${scratch}/uncbin/wslpath"
    printf 'gitdir: //localhost/fc-test-share%s\n' "${repo}/.git/worktrees/lane" > "${wt}/.git"
    PATH="${scratch}/uncbin:${PATH}" repair_worktree "$wt" apply >/dev/null 2>&1
    local uncStatus=$?
    wtPointer="$(cat "${wt}/.git")"
    case "$wtPointer" in
        gitdir:\ /*|gitdir:\ [A-Za-z]:[/\\]*) verdict=no ;;
        gitdir:\ ?*)                        verdict=yes ;;
        *)                                  verdict=no ;;
    esac
    _rw_expect "a forward-slash UNC pointer to a WSL-native repository REPAIRS, and git reads it" \
        "0 yes yes" "${uncStatus} ${verdict} $(git_enumerates "$wt" && echo yes || echo no)"

    rm -rf "$scratch"
    echo "repair-worktree-pointers --self-test: ${ran} checks ran, ${failed} failed"
    if [ "$ran" -ne "$RepairSelfTestChecks" ]; then
        echo "REPAIR SELF-TEST FAILED: ran ${ran} checks where it declares ${RepairSelfTestChecks}: it stopped early, skipped a block or lost a row -- or RepairSelfTestChecks was not updated with one" >&2
        return 1
    fi
    [ "$failed" -eq 0 ] || return 1
    echo "REPAIR SELF-TEST PASSED"
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
else
    # Absolute, in this shell's own spelling: `.` or `../lane` typed at a prompt reached
    # `relative_path` as typed, which can only compute from absolute paths.
    repair_root_typed="$repair_root"
    repair_root="$(cd "$repair_root" 2>/dev/null && pwd)" || {
        echo "worktree: cannot enter '${repair_root_typed}', so there is no work tree to look at." >&2
        exit 2
    }
fi

repair_worktree "$repair_root" "$repair_apply"
