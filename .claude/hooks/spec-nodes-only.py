#!/usr/bin/env python3
"""PreToolUse hook: the spec is read and written as nodes, never as text.

The spec is the XML under spec/articles/, which tools/spec/spec reads with
queries and writes with node updates that validate every change. The
pages under docs/ that it generates (each document's `file`) are its
output. Text tools on either (Read, Edit, Write, Grep; cat, sed, grep,
awk, python, cp, redirection, ...) are refused, with the command that does
the same with nodes. Programs that only list names (ls, wc, du, find
without an action), git, the sxml command and tools/spec itself may name
them, and searches of the whole repository still run.

Exit code 2 blocks the tool call and shows stderr to the agent.
"""
import glob
import json
import os
import re
import shlex
import sys

ARTICLES = "spec/articles"
PAGE_DIRECTORIES = ("docs", "docs/tasks", "docs/agents")
# Programs that may name the spec's files: they work on nodes, or only on names.
ALLOWED = {"tools/spec/spec", "spec", "sxml", "git", "ls", "wc", "du", "find", "mise", "true"}
HELP = (
    "\nRead:   tools/spec/spec show NAME [--depth=2], refs NAME, brief PACKET, find WORDS,"
    "\n        text NAME, markdown DOCUMENT, list KIND, status, or any XPath:"
    "\n        tools/spec/spec query '$articles/decision[@id = \"D238\"]'"
    "\nWrite:  tools/spec/spec amend NAME --summary=... | --body=-, add decision|term|packet ...,"
    "\n        log TEXT, set-status PACKET done, supersede, rename TERM NEW,"
    "\n        update NAME 'XQUERY UPDATE', apply EXPRESSION"
    "\nSee tools/spec/spec help and spec/README.md.\n"
)

payload = json.load(sys.stdin)
tool_name = payload.get("tool_name", "")
tool_input = payload.get("tool_input", {}) or {}
project = os.path.normpath(os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or os.getcwd())


def block(reason):
    sys.stderr.write(
        "Blocked by .claude/hooks/spec-nodes-only.py: " + reason
        + " The spec (spec/articles/ and the pages it generates under docs/) is read"
        " and written as nodes through tools/spec/spec, never as text." + HELP
    )
    sys.exit(2)


def generated_pages():
    pages = set()
    for path in glob.glob(os.path.join(project, ARTICLES, "documents", "**", "*.xml"), recursive=True):
        with open(path, encoding="utf-8") as source:
            match = re.search(r'\bfile="([^"]+)"', source.readline())
        if match:
            pages.add(os.path.normpath(os.path.join(project, match.group(1))))
    return pages


PAGES = generated_pages()


def protected(path, base=project):
    """True for an article, a generated page, or a directory of pages."""
    if not path:
        return False
    absolute = os.path.normpath(os.path.join(base, os.path.expanduser(path)))
    articles = os.path.join(project, ARTICLES)
    return (absolute == articles or absolute.startswith(articles + os.sep) or absolute in PAGES
            or absolute in {os.path.join(project, directory) for directory in PAGE_DIRECTORIES})


if tool_name in ("Read", "Edit", "Write", "MultiEdit", "NotebookEdit"):
    path = tool_input.get("file_path") or tool_input.get("notebook_path")
    if protected(path):
        block(tool_name + " on " + os.path.relpath(os.path.join(project, path), project) + ".")
    sys.exit(0)

if tool_name == "Grep":
    if protected(tool_input.get("path")) or any(
            ARTICLES in text or re.search(r"docs/(tasks/|agents/)?[\w*.-]*\.md", text)
            for text in (tool_input.get("glob") or "",)):
        block("Grep over the spec.")
    sys.exit(0)

if tool_name != "Bash":
    sys.exit(0)

command = tool_input.get("command", "")
directory = os.path.normpath(payload.get("cwd") or project)
if "articles" not in command and "docs" not in command and not protected(directory):
    sys.exit(0)


def names_spec(word):
    return ARTICLES in word or protected(word, directory)


touched = False  # an earlier program named the spec's files: xargs gets them
for segment in re.split(r"&&|\|\||;|\||\n|\$\(|`", command):
    if any(names_spec(target) for target in re.findall(r">>?\s*['\"]?([^\s;|&'\"]+)", segment)):
        block("a shell redirection writes into the spec.")
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
    inside = directory.startswith(os.path.join(project, ARTICLES))
    touches = inside or any(names_spec(word) for word in words[1:]) or (from_pipe and touched)
    if not touches:
        continue
    touched = True
    name = program if program.startswith("tools/") else os.path.basename(program)
    if name not in ALLOWED:
        block("'" + program + "' on the spec.")
    if name == "find" and any(word in ("-exec", "-execdir", "-delete", "-ok", "-fprint") for word in words):
        block("find with an action on the spec.")

sys.exit(0)
