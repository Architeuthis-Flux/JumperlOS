#!/bin/sh
# One-time repo-cleanup finisher (2026-08-24). These are the steps the
# sandbox refused to run for Claude (they delete directories / uncommitted
# changes / remote branches). Everything here was verified content-safe:
# - the two Cursor worktrees are Oct-2025 scratch copies whose useful edits
#   (i2cScan internalScan param, the main.cpp services refactor) shipped
#   long ago
# - the serene-burnell agent worktree's uncommitted configManager fix +
#   droop HIL test are byte-for-byte present in dev already
# - stable-5.7.5 is fully merged into origin/main (PR #10); its checkout
#   has old submodules, which is why `git worktree remove` refuses
# - every deleted branch with any unique commit is preserved under a local
#   tag: git tag -l 'archive/*'
# Run from /Users/kevinsanto/Documents/GitHub/JumperlOS, then delete me.
set -x

git worktree remove --force "/Users/kevinsanto/.cursor/worktrees/JumperlOS__Workspace_/6SPu6"
git worktree remove --force "/Users/kevinsanto/.cursor/worktrees/JumperlOS__Workspace_/Wiiec"
git branch -D 2025-10-26-8pyb-6SPu6 2025-10-26-2q1v-Wiiec

rm -rf "/Users/kevinsanto/Documents/GitHub/JumperlOS-stable-5.7.5"
git worktree prune
git branch -D stable-5.7.5

git worktree remove --force ".claude/worktrees/serene-burnell-b7797c"
git branch -D claude/serene-burnell-b7797c

# Remote ends up main + dev only.
git push origin --delete projects-guided-placement FlashyFix MeasureModePlus \
    OGbackport probe-refactor stable-5.7.5 undo

git worktree list
git branch -a
