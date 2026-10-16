#!/usr/bin/env python3
"""PreToolUse hook: all work happens on main. Blocks branch and worktree creation.

Exit code 2 blocks the tool call and shows stderr to the agent.
"""
import json
import re
import shlex
import sys

payload = json.load(sys.stdin)
tool_name = payload.get("tool_name", "")
tool_input = payload.get("tool_input", {}) or {}


def block(reason):
    sys.stderr.write(
        "Blocked by .claude/hooks/main-only.py: " + reason
        + " All sxml work is committed and pushed directly on main;"
        " do not create branches or worktrees.\n"
    )
    sys.exit(2)


if tool_name in ("EnterWorktree", "mcp__github__create_branch"):
    block(tool_name + " creates a branch or worktree.")

if tool_name == "Agent" and tool_input.get("isolation") == "worktree":
    block("Agent isolation 'worktree' creates a branch.")

if tool_name != "Bash":
    sys.exit(0)

command = tool_input.get("command", "")

# Split the command line into simple commands at shell separators.
for segment in re.split(r"&&|\|\||;|\||\n", command):
    try:
        words = shlex.split(segment)
    except ValueError:
        words = segment.split()
    # Drop shell redirections such as `2>&1` or `> file`.
    cleaned = []
    skip_next = False
    for word in words:
        if skip_next:
            skip_next = False
            continue
        if re.match(r"^(\d*|&)[<>]", word):
            skip_next = re.match(r"^(\d*|&)[<>]+&?$", word) is not None and not word.endswith(("&1", "&2"))
            continue
        cleaned.append(word)
    words = cleaned
    # Drop leading environment assignments and `git -C dir` style options.
    while words and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]):
        words.pop(0)
    if len(words) < 2 or words[0] != "git":
        continue
    arguments = words[1:]
    while arguments and arguments[0].startswith("-"):
        option = arguments.pop(0)
        if option in ("-C", "-c") and arguments:
            arguments.pop(0)
    if not arguments:
        continue
    subcommand, rest = arguments[0], arguments[1:]

    if subcommand == "checkout" and any(flag in rest for flag in ("-b", "-B", "--orphan")):
        block("git checkout -b creates a branch.")
    if subcommand == "switch":
        if any(flag in rest for flag in ("-c", "-C", "--create", "--force-create", "--orphan")):
            block("git switch -c creates a branch.")
        targets = [word for word in rest if not word.startswith("-")]
        if targets and targets[0] != "main":
            block("only main may be checked out.")
    if subcommand == "branch":
        listing_flags = {"-a", "--all", "-r", "--remotes", "-l", "--list", "-v", "-vv",
                         "--show-current", "--contains", "--merged", "--no-merged",
                         "-d", "-D", "--delete", "--format", "--sort", "-u",
                         "--set-upstream-to", "--unset-upstream"}
        positional = [word for word in rest if not word.startswith("-")]
        if positional and not any(word in listing_flags or word.startswith("--format") or word.startswith("--set-upstream-to") for word in rest):
            block("git branch <name> creates a branch.")
    if subcommand == "worktree" and rest and rest[0] == "add":
        block("git worktree add creates a worktree.")
    if subcommand == "push":
        positional = [word for word in rest if not word.startswith("-")]
        refspecs = positional[1:]
        for refspec in refspecs:
            destination = refspec.split(":")[-1].lstrip("+")
            if destination not in ("main", "refs/heads/main", "HEAD") and not refspec.startswith(":"):
                block("pushing to '" + destination + "'; only main may be pushed.")
            if refspec.startswith(":") and refspec != ":":
                # Deleting a remote branch is allowed only by the human.
                block("deleting remote refs is not allowed from agents.")

sys.exit(0)
