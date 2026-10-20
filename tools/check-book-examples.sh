#!/bin/sh
# Re-runs every example of the sxml book (docs/book) and compares its output
# with the output the book shows, so the book stays true.
#
# The book shows commands in ```console blocks: a line that starts with "$ "
# is a command, which continues on the next line after a final "\" or while
# a quotation is open; the lines up to the next command or the end of the
# block are its output, standard output and standard error together.
#
# Each chapter file names its example directory with a comment
#   <!-- examples: NAME -->
# The commands of that file run, in order, in one fresh copy of
# docs/book/examples/NAME (an empty directory without the comment), so a
# file one command writes is there for the next. Each command runs as
# `sh -c COMMAND` with standard input from /dev/null and these variables:
#   PATH        the directory of the sxml command first
#   SXML        the sxml source the C chapter builds: the repository root,
#               unless SXML is set already (to a snapshot of HEAD, say)
#
# A chapter file with the comment <!-- requires: javascript --> runs its
# examples with Node.js against the JavaScript package: set
# SXML_JAVASCRIPT to a built package directory (bindings/javascript after
# `npm run build`, with dist/), which is linked into the copy as
# node_modules/@tinyactors/sxml. Without SXML_JAVASCRIPT those chapters
# are skipped, and the summary says so.
#
# A chapter file with the comment <!-- status: draft --> describes a
# feature before it is built: the book comes first, its examples show the
# output the feature will give, and they become the feature's acceptance
# test. Draft chapters are skipped (the summary names them) unless
# --drafts is given; the commit that makes a draft's examples pass removes
# the comment.
#
# Usage: tools/check-book-examples.sh [--update] [--drafts] [BUILD-DIRECTORY] [CHAPTER.md...]
#   BUILD-DIRECTORY defaults to build/release; it must hold apps/sxml.
#   CHAPTER.md files (paths below docs/book/src) limit the check to them.
#   --update writes the actual outputs into the chapter files instead of
#   failing; review the diff before committing it.
# Exit status: 0 when every example matches, 1 otherwise, 2 for bad usage.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 -I - "$repository_root" "$@" <<'PYTHON'
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

repository_root = sys.argv[1]
arguments = sys.argv[2:]
update = False
check_drafts = False
while arguments and arguments[0] in ("--update", "--drafts"):
    if arguments[0] == "--update":
        update = True
    else:
        check_drafts = True
    arguments = arguments[1:]
build_directory = os.path.join(repository_root, "build", "release")
if arguments and not arguments[0].endswith(".md"):
    build_directory = os.path.abspath(arguments[0])
    arguments = arguments[1:]
sxml_command = os.path.join(build_directory, "apps", "sxml")
if not os.access(sxml_command, os.X_OK):
    print(f"check-book-examples: no sxml command at {sxml_command}", file=sys.stderr)
    sys.exit(2)

book_source = os.path.join(repository_root, "docs", "book", "src")
examples_root = os.path.join(repository_root, "docs", "book", "examples")
if arguments:
    chapter_files = [os.path.join(book_source, name) for name in arguments]
else:
    chapter_files = sorted(
        os.path.join(directory, name)
        for directory, _, names in os.walk(book_source)
        for name in names if name.endswith(".md"))

environment = dict(os.environ)
environment["PATH"] = os.path.dirname(sxml_command) + os.pathsep + environment.get("PATH", "")
environment.setdefault("SXML", repository_root)
environment["LC_ALL"] = "C.UTF-8"
environment["TZ"] = "UTC"


def is_continued(command):
    """A command continues on the next line after a final backslash, or
    while a quotation in it is still open."""
    if command.endswith("\\"):
        return True
    try:
        shlex.split(command)
    except ValueError:
        return True
    return False


def parse_examples(lines):
    """Yields (first line index, last line index + 1, command, expected
    output lines) for each command in the ```console blocks."""
    in_console = False
    current = None
    for line_index, line in enumerate(lines):
        stripped = line.rstrip("\n")
        if not in_console:
            if stripped.strip() == "```console":
                in_console = True
            continue
        if stripped.strip() == "```":
            if current is not None:
                yield current
                current = None
            in_console = False
            continue
        if current is not None and current["continued"]:
            current["command"] += "\n" + stripped
            current["continued"] = is_continued(current["command"])
            current["output_start"] = line_index + 1
            continue
        if stripped.startswith("$ "):
            if current is not None:
                yield current
            current = {
                "line": line_index,
                "command": stripped[2:],
                "continued": is_continued(stripped[2:]),
                "output_start": line_index + 1,
                "output": [],
            }
            continue
        if current is not None:
            current["output"].append(stripped)
    if current is not None:
        yield current


def requires_javascript(lines):
    return any(line.strip() == "<!-- requires: javascript -->" for line in lines)


def is_draft(lines):
    return any(line.strip() == "<!-- status: draft -->" for line in lines)


def examples_directory_of(lines):
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("<!-- examples:") and stripped.endswith("-->"):
            return stripped[len("<!-- examples:"):-len("-->")].strip()
    return None


failures = 0
command_count = 0
skipped_chapters = []
draft_chapters = []
javascript_package = os.environ.get("SXML_JAVASCRIPT", "")
for chapter_file in chapter_files:
    with open(chapter_file, encoding="utf-8") as chapter:
        lines = chapter.readlines()
    examples = list(parse_examples(lines))
    if not examples:
        continue
    relative_chapter = os.path.relpath(chapter_file, repository_root)
    if is_draft(lines) and not check_drafts:
        draft_chapters.append(relative_chapter)
        continue
    needs_javascript = requires_javascript(lines)
    if needs_javascript and not javascript_package:
        skipped_chapters.append(relative_chapter)
        continue
    name = examples_directory_of(lines)
    work_root = tempfile.mkdtemp(prefix="sxml-book-")
    work_directory = os.path.join(work_root, name or "empty")
    if name:
        source = os.path.join(examples_root, name)
        if not os.path.isdir(source):
            print(f"{relative_chapter}: no example directory {source}", file=sys.stderr)
            failures += 1
            continue
        shutil.copytree(source, work_directory)
    else:
        os.makedirs(work_directory)
    if needs_javascript:
        package_link = os.path.join(work_directory, "node_modules", "@tinyactors", "sxml")
        os.makedirs(os.path.dirname(package_link), exist_ok=True)
        os.symlink(os.path.abspath(javascript_package), package_link)
    replacements = []
    for example in examples:
        command_count += 1
        try:
            completed = subprocess.run(
                ["sh", "-c", example["command"]], cwd=work_directory,
                env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900)
            actual = completed.stdout.decode("utf-8", errors="replace")
        except subprocess.TimeoutExpired:
            actual = "(timed out after 900 seconds)\n"
        actual_lines = [line.rstrip() for line in actual.rstrip("\n").split("\n")] if actual.strip("\n") else []
        expected_lines = [line.rstrip() for line in example["output"]]
        while expected_lines and expected_lines[-1] == "":
            expected_lines.pop()
        if actual_lines != expected_lines:
            if update:
                replacements.append((example["output_start"],
                                     example["output_start"] + len(example["output"]),
                                     actual_lines))
            else:
                failures += 1
                print(f"{relative_chapter}:{example['line'] + 1}: $ {example['command']}")
                print("  expected:")
                for line in expected_lines:
                    print("    " + line)
                print("  actual:")
                for line in actual_lines:
                    print("    " + line)
    shutil.rmtree(work_root)
    if replacements:
        for start, end, actual_lines in reversed(replacements):
            lines[start:end] = [line + "\n" for line in actual_lines]
        with open(chapter_file, "w", encoding="utf-8") as chapter:
            chapter.writelines(lines)
        print(f"{relative_chapter}: updated {len(replacements)} outputs")

for chapter in draft_chapters:
    print(f"check-book-examples: {chapter} is a draft; --drafts checks it")
for chapter in skipped_chapters:
    print(f"check-book-examples: {chapter} skipped; set SXML_JAVASCRIPT to check it")
if failures:
    print(f"check-book-examples: {failures} of {command_count} examples differ", file=sys.stderr)
    sys.exit(1)
print(f"check-book-examples: all {command_count} examples match")
PYTHON
