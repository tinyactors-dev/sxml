#!/usr/bin/env python3
"""PreToolUse hook: the spec's articles are read and written as nodes, never as text.

spec/articles/ is XML that tools/spec/spec reads with queries and writes
with node updates, which validate every change and keep each file in
canonical form (spec/README.md). Text tools on those files (Read, Edit,
Write, Grep; cat, sed, grep, awk, python, ...) are refused, with the
command that does the same thing on nodes. Programs that only name the
files (ls, wc, du), git, the sxml command and tools/spec itself may.

Exit code 2 blocks the tool call and shows stderr to the agent.
"""
import json
import os
import re
import shlex
import sys

ARTICLES = "spec/articles"
# Programs that may name article files: they work on nodes, or only on names.
ALLOWED = {"tools/spec/spec", "spec", "sxml", "git", "ls", "wc", "du", "find", "xargs", "mise", "true"}
HELP = (
    "\nRead:   tools/spec/spec show NAME [--depth=2], refs NAME, brief PACKET, find WORDS,"
    "\n        list KIND, status, or any XPath: tools/spec/spec query '$articles/decision[@id=\"D238\"]'"
    "\nWrite:  tools/spec/spec amend NAME --summary=..., add decision|term|packet ..., supersede,"
    "\n        rename TERM NEW, set-status PACKET done, update NAME 'XQUERY UPDATE', apply EXPRESSION"
    "\nSee tools/spec/spec help and spec/README.md.\n"
)

payload = json.load(sys.stdin)
tool_name = payload.get("tool_name", "")
tool_input = payload.get("tool_input", {}) or {}
project = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or os.getcwd()


def block(reason):
    sys.stderr.write(
        "Blocked by .claude/hooks/spec-nodes-only.py: " + reason
        + " Files under spec/articles/ are read and written as nodes through"
        " tools/spec/spec, never as text." + HELP
    )
    sys.exit(2)


def in_articles(path):
    if not path:
        return False
    absolute = os.path.normpath(os.path.join(project, os.path.expanduser(path)))
    articles = os.path.normpath(os.path.join(project, ARTICLES))
    return absolute == articles or absolute.startswith(articles + os.sep)


if tool_name in ("Read", "Edit", "Write", "MultiEdit", "NotebookEdit"):
    path = tool_input.get("file_path") or tool_input.get("notebook_path")
    if in_articles(path):
        block(tool_name + " on " + os.path.relpath(path, project) + ".")
    sys.exit(0)

if tool_name in ("Grep", "Glob"):
    path = tool_input.get("path") or ""
    pattern = tool_input.get("glob") or tool_input.get("pattern") or ""
    if in_articles(path) or (tool_name == "Glob" and ARTICLES in pattern) or (tool_name == "Grep" and ARTICLES in (tool_input.get("glob") or "")):
        block(tool_name + " over spec/articles/.")
    sys.exit(0)

if tool_name != "Bash":
    sys.exit(0)

command = tool_input.get("command", "")
if "articles" not in command and not in_articles(payload.get("cwd") or project):
    sys.exit(0)
directory = payload.get("cwd") or project


def names_articles(word):
    return ARTICLES in word or in_articles(os.path.join(directory, word))


touched = False  # an earlier program named article files: xargs gets them
for segment in re.split(r"&&|\|\||;|\||\n|\$\(|`", command):
    if any(names_articles(target) for target in re.findall(r">>?\s*['\"]?([^\s;|&'\"]+)", segment)):
        block("a shell redirection writes into spec/articles/.")
    try:
        words = shlex.split(segment)
    except ValueError:
        words = segment.split()
    while words and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]):
        words.pop(0)
    from_pipe = False
    while words and words[0] in ("sudo", "env", "command", "exec", "time", "nice", "xargs", "("):
        from_pipe = from_pipe or words[0] == "xargs"
        words.pop(0)
        while words and words[0].startswith("-"):
            words.pop(0)
    if not words:
        continue
    program = words[0]
    if program in ("cd", "pushd"):
        if len(words) > 1:
            directory = os.path.normpath(os.path.join(directory, os.path.expanduser(words[1])))
        continue
    touches = in_articles(directory) or any(names_articles(word) for word in words[1:]) or (from_pipe and touched)
    if not touches:
        continue
    touched = True
    name = program if program.startswith("tools/") else os.path.basename(program)
    if name not in ALLOWED:
        block("'" + program + "' on spec/articles/.")
    if name == "find" and any(word in ("-exec", "-execdir", "-delete", "-ok", "-fprint") for word in words):
        block("find with an action on spec/articles/.")

sys.exit(0)
