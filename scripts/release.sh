#!/usr/bin/env bash
# Cut a release.
#
#   ./scripts/release.sh 0.2.1
#   ./scripts/release.sh 0.2.1 --skip-vm     # only when qemu is unavailable
#
# Validates the tree, bumps ./VERSION, commits (you write the message), tags,
# and pushes. Stops at the first thing that fails, and nothing leaves this
# machine until the final confirmation.
#
# ---------------------------------------------------------------------------
# What is under test, and what is not
#
# The pre-release run tests THE WORKING TREE. The version number is not part of
# it and deliberately does not have to be correct yet — a build stamped with the
# outgoing version is testing exactly the same code. Checking that ./VERSION and
# the tag agree is a separate, later concern (`make check-version`), and
# conflating the two would mean either testing the wrong thing or refusing to
# test at all in the perfectly ordinary state of "bumped but not yet committed".
#
# Why the steps are in this order
#
# The obvious order — bump, tag, commit, push — tags the WRONG COMMIT. A tag
# points at a commit, so the release commit has to exist before the tag can name
# it. Tagging first leaves the tag on the previous release, and `check-version`
# then fails because HEAD is no longer the tagged commit.
#
# Build and test run BEFORE the bump for a second reason: between bumping and
# committing, HEAD is still the previous release's tagged commit, so
# check-version would fire on a state that is correct and momentary.
#
# The build runs again after tagging. That one is not redundant: it is where
# check-version becomes a real gate, and it is the first build whose stamped
# version is the release version.
#
# New commit, or amend
#
# If HEAD already carries unreleased work — i.e. it is not the commit that last
# touched ./VERSION — the bump is folded into that commit by amending, rather
# than trailing a separate "release X.Y.Z" commit behind it. A release commit
# that exists only to change one line is noise, and it puts the tag on a commit
# that contains none of what is being released.
#
# Amending is skipped when HEAD is already published, because rewriting pushed
# history is a different and much worse problem than an extra commit.
# ---------------------------------------------------------------------------
set -euo pipefail

BRANCH=master
REMOTE=origin

die() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
ask() { # prompt -> 0 if the answer began with y
    local reply
    read -r -p "$1 [y/N] " reply
    [[ ${reply,,} == y* ]]
}

# --- arguments --------------------------------------------------------------

V=
SKIP_VM=0
for arg in "$@"; do
    case $arg in
        --skip-vm) SKIP_VM=1 ;;
        -*) die "unknown option '$arg'. usage: $(basename "$0") X.Y.Z [--skip-vm]" ;;
        *) [ -z "$V" ] || die "give exactly one version. usage: $(basename "$0") X.Y.Z [--skip-vm]"
           V=$arg ;;
    esac
done

[ -n "$V" ] || die "usage: $(basename "$0") X.Y.Z [--skip-vm]"
[[ $V =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "'$V' is not X.Y.Z. The tag becomes v$V and a GitHub release is cut from it, so it has to be plain semver."

cd "$(dirname "${BASH_SOURCE[0]}")/.." || die "cannot find the repo root"
[ -f VERSION ] || die "no ./VERSION here — is this the right directory?"

# --- preflight --------------------------------------------------------------
#
# Everything cheap that can fail, before anything expensive or irreversible.

step "Preflight"

CURRENT=$(cat VERSION)
echo "  current version: $CURRENT"
echo "  releasing:       $V"

[ "$V" != "$CURRENT" ] || die "./VERSION already says $V. Nothing to bump — if you are re-running after a failure, finish by hand rather than starting over."

# String compare is enough to catch the common slip of going backwards; it is
# not a full semver comparison and does not pretend to be.
if [ "$(printf '%s\n%s\n' "$CURRENT" "$V" | sort -V | tail -1)" != "$V" ]; then
    ask "  $V looks older than $CURRENT. Continue anyway?" || die "aborted"
fi

git rev-parse --git-dir > /dev/null 2>&1 || die "not a git repository"

ON=$(git branch --show-current)
[ "$ON" = "$BRANCH" ] || die "on branch '$ON', not '$BRANCH'. Releases are cut from $BRANCH."

# A tag is effectively immutable once pushed: git refuses to move it, and anyone
# who already fetched keeps the old one. So refuse early rather than discover it
# at push time with a commit and a tag already made.
! git rev-parse -q --verify "refs/tags/v$V" > /dev/null || die "tag v$V already exists locally. Pick the next patch version, or delete it if it was never pushed: git tag -d v$V"

# Deliberately NOT `ls-remote --exit-code <pattern>`. That returns 2 both for
# "no such tag" and, at least in some failure modes, for "never reached the
# remote" — so a network blip would read as "safe to release", which is the one
# moment you want this check to speak up.
#
# Instead: the command's own success is the reachability signal, and the tag is
# looked for in the output. Exact field match, so v0.2.1 is not satisfied by
# v0.2.10.
if ! REMOTE_TAGS=$(git ls-remote --tags "$REMOTE" 2>/dev/null); then
    ask "  could not reach $REMOTE to check whether v$V is already published. Continue?" || die "aborted"
elif printf '%s\n' "$REMOTE_TAGS" | awk -v t="refs/tags/v$V" '$2 == t { found = 1 } END { exit !found }'; then
    die "tag v$V is already published on $REMOTE. Published tags do not move — release v${V%.*}.$((${V##*.} + 1)) instead."
fi

git diff --quiet --exit-code -- VERSION || die "./VERSION has uncommitted edits. This script owns that file during a release; commit or revert them first."

# --- new commit, or amend the one already at HEAD ---------------------------
#
# "The last commit with a version" is the one that last touched ./VERSION, i.e.
# the previous release commit. If HEAD is something newer, that work is what is
# being released and the bump belongs *in* it.
git fetch --quiet "$REMOTE" 2>/dev/null || echo "  (could not fetch $REMOTE; the published-history check below may be stale)"

VERSION_COMMIT=$(git log -1 --format=%H -- VERSION 2>/dev/null || true)
HEAD_SHA=$(git rev-parse HEAD)
DIRTY=$(git status --porcelain | head -1)
MODE=new

if [ -n "$VERSION_COMMIT" ] && [ "$VERSION_COMMIT" != "$HEAD_SHA" ]; then
    if git merge-base --is-ancestor "$HEAD_SHA" "$REMOTE/$BRANCH" 2>/dev/null; then
        # Folding the bump into a pushed commit would rewrite published history.
        MODE=new
        echo "  HEAD is already published, so the bump becomes its own commit"
    else
        MODE=amend
        echo "  HEAD carries unreleased work and is unpushed — the bump will be amended into it:"
        echo "      $(git log -1 --oneline)"
    fi
else
    echo "  HEAD is the last release commit — the bump becomes a new commit"
fi

echo "  ok"

# --- validate ---------------------------------------------------------------

step "Building and testing the working tree"
echo "  ($(git log -1 --oneline)${DIRTY:+, plus uncommitted changes})"
echo "  The stamped version is irrelevant here — this is testing the code."
make test
make plugin

step "Running the in-compositor suite"
if [ "$SKIP_VM" = 1 ]; then
    printf '  \033[1;33mSKIPPED (--skip-vm).\033[0m The unit suites never touch a compositor, so\n'
    printf '  nothing verified above covers the hook, the config path or startup.\n'
    ask "  Release without in-compositor verification?" || die "aborted"
else
    # vm-up is idempotent: fetch no-ops on an existing image, and start reports
    # an already-running VM rather than booting a second one.
    make vm-up
    make vm-verify
    echo
    echo "  The VM is left running; 'make vm-down' stops it."
fi

# --- from here on there is state to unwind ----------------------------------

COMMITTED=0
TAGGED=0
AMEND_FROM=

# Written with explicit if-blocks rather than `cond && printf`. Under `set -e` a
# false `cond &&` is a failing command inside the trap, which would abort the
# handler partway and swallow the rest of the advice — precisely when it is
# needed. This runs only on the failure path, so it never gets exercised
# casually; it has to be right by construction.
on_failure() {
    local code=$?
    trap - EXIT

    if [ "$code" -eq 0 ]; then
        exit 0
    fi
    if [ "$COMMITTED" = 0 ] && [ "$TAGGED" = 0 ]; then
        exit "$code"
    fi

    printf '\n\033[1mFailed with local state already made.\033[0m Nothing was pushed.\n'
    printf 'To unwind:\n'
    if [ "$TAGGED" = 1 ]; then
        printf '    git tag -d v%s\n' "$V"
    fi
    if [ "$COMMITTED" = 1 ]; then
        if [ -n "$AMEND_FROM" ]; then
            # HEAD replaced a commit rather than adding one, so HEAD~1 would
            # throw away the work that was amended into it.
            printf '    git reset --soft %s   # restores the commit that was amended\n' "$AMEND_FROM"
        else
            printf '    git reset --soft HEAD~1     # keeps your changes staged\n'
        fi
    fi
    printf '\n'
    exit "$code"
}
trap on_failure EXIT

step "Bumping ./VERSION to $V"
printf '%s\n' "$V" > VERSION

step "Review the release commit"

if [ "$MODE" = amend ]; then
    echo "  Folding into HEAD:"
    echo "      $(git log -1 --oneline)"
    echo
    echo "  Changes to fold in:"
    git status --short | sed 's/^/    /'
    echo
    echo "  Your editor then opens on that commit's existing message — leave it,"
    echo "  or adjust it now that it carries the release."
    ask "  Fold in and amend?" || die "aborted before committing (./VERSION is now $V — revert it if you are stopping here)"
else
    echo "  New commit will contain:"
    git status --short | sed 's/^/    /'
    echo
    echo "  Your editor then opens, prefilled with \"release v$V\"."
    ask "  Stage and commit?" || die "aborted before committing (./VERSION is now $V — revert it if you are stopping here)"
fi

git add -A

# Clearing the message in the editor aborts the commit, and git's own exit code
# brings the whole script down with it.
if [ "$MODE" = amend ]; then
    AMEND_FROM=$HEAD_SHA
    git commit --amend
else
    git commit -e -m "release v$V"
fi
COMMITTED=1

step "Tagging v$V"
# Annotated, not lightweight: `git push --follow-tags` skips lightweight tags
# entirely, and GitHub shows the annotation on the release page. The tag message
# reuses the commit subject so the two never tell different stories.
git tag -a "v$V" -m "$(git log -1 --format=%s)"
TAGGED=1

step "Rebuilding at the release version"
# The real check-version gate: ./VERSION and the tag on HEAD must now agree.
# Also the first build whose stamped version is $V rather than $CURRENT.
make plugin

step "Ready to push"
echo "  commit: $(git log -1 --oneline)"
echo "  tag:    v$V -> $(git rev-parse --short "v$V^{commit}")"
echo "  remote: $REMOTE $(git remote get-url "$REMOTE")"
echo
echo "  This publishes. --follow-tags sends $BRANCH plus annotated tags reachable"
echo "  from it, and nothing else; a plain --tags would push every stray local tag"
echo "  and no branch at all."
ask "  Push to $REMOTE/$BRANCH?" || die "not pushed. The commit and tag exist locally; push when ready with: git push --follow-tags $REMOTE $BRANCH"

git push --follow-tags "$REMOTE" "$BRANCH"

trap - EXIT
step "Released v$V"
echo "  Draft the GitHub release against the tag when you are ready."
