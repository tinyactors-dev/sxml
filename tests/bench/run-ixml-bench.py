#!/usr/bin/env python3
"""Runs the Invisible XML benchmarks (packets 7.21 and 7.24) and checks
the claims of the book's chapter "Invisible XML Performance"
(docs/book/src/ixml-performance.md).

Usage: tests/bench/run-ixml-bench.py (--build DIR | --program PATH)
                                     [--repeat N] [--only NAME]... [--check]
                                     [--large]

  --build DIR     a build directory (the release preset: build/release)
                  with apps/sxml
  --program PATH  the sxml command itself (another build, to compare)
  --repeat N      runs of each benchmark; the fastest is reported (default 3)
  --only NAME     run only the benchmarks named (repeatable)
  --check         exit 1 unless every benchmark keeps its limits (the
                  book's claims): processor seconds, most resident memory,
                  the memory each further MB of input takes (from the
                  smaller input to the larger), and, for the grammars the
                  parser is linear on, the growth of the time from the
                  smaller input to the one four times larger
  --large         also run the log of 100 MB (some 15 s and 2 GB a run)

Each benchmark parses a generated input (made here, the same bytes every
time) with a grammar of tests/bench/ixml, in one process of
`sxml xquery`, and prints the number of elements of the result and its
ixml:state, so two builds can be compared. Times are processor seconds
(user plus system, from wait4), the fastest of the runs: wall-clock time
is too noisy on a machine that other builds share. They include starting
the process, reading the input and building the XML tree.

The comparisons parse the same kind of input with the parsers the book
compares iXML with: a JSON grammar of tests/bench/ixml against
tree-sitter:parse and fn:json-to-xml, and the Markdown input
with tree-sitter:parse; they print the number of elements.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

BENCH = os.path.dirname(os.path.abspath(__file__))
GRAMMARS = os.path.join(BENCH, "ixml")

QUERY = """declare variable $grammar external;
declare variable $input external;
let $document := invisible-xml(unparsed-text($grammar))(unparsed-text($input))
return (count($document//*), string($document/*/@*:state))"""

GRAMMAR_QUERY = """declare variable $grammar external;
declare variable $input external;
let $parse := invisible-xml(unparsed-text($grammar))
return count($parse(unparsed-text($input))//*)"""


def letters(number):
    """number in the letters a to z, so it is a name of calc.ixml."""
    text = ""
    while True:
        text = chr(ord("a") + number % 26) + text
        number //= 26
        if number == 0:
            return text


def make_log(size):
    levels = ["INFO", "WARN", "ERROR"]
    lines = []
    total = 0
    index = 0
    while total < size:
        line = "2026-10-20 %02d:%02d:%02d %s message number %d with some text\n" % (
            index // 3600 % 24, index // 60 % 60, index % 60, levels[index % 3], index)
        lines.append(line)
        total += len(line)
        index += 1
    return "".join(lines)


def make_csv(size):
    rows = ["item;price;note\n"]
    total = len(rows[0])
    index = 0
    while total < size:
        row = '"item %d; boxed";%d.%02d;%s\n' % (index, index % 100, index % 97,
                                                  "note" if index % 3 else "")
        rows.append(row)
        total += len(row)
        index += 1
    return "".join(rows)


def make_calc(size):
    statements = []
    total = 0
    index = 0
    while total < size:
        name = letters(index)
        previous = letters(max(index - 1, 0))
        if index == 0:
            statement = "let a = 1;\n"
        else:
            statement = "let %s = (%s + %d) * 3 - %s / 2;\nprint %s;\n" % (
                name, previous, index % 10, previous, name)
        statements.append(statement)
        total += len(statement)
        index += 1
    return "".join(statements)


def make_markdown(size):
    blocks = []
    total = 0
    index = 0
    while total < size:
        block = ("## Section %d\n\nThis paragraph has *emphasis* and `code` in it,\n"
                 "and a second line.\n\n- first item\n- second *item*\n\n" % index)
        blocks.append(block)
        total += len(block)
        index += 1
    return "# Notes\n\n" + "".join(blocks).rstrip("\n") + "\n"


def make_numbers(size):
    parts = []
    total = 0
    index = 1
    while total < size:
        part = str(index)
        parts.append(part)
        total += len(part) + 1
        index += 1
    return ",".join(parts) + "\n"


def make_json(size):
    """Feature flags as indented JSON: objects, arrays, strings, numbers,
    booleans and nulls."""
    flags = []
    total = 0
    index = 0
    while total < size:
        flag = ('  {\n    "id": %d,\n    "name": "flag %d",\n    "enabled": %s,\n'
                '    "rollout": %d,\n    "owners": ["team-%d", "team-%d"],\n    "note": %s\n  }'
                % (index, index, "true" if index % 3 == 0 else "false", index % 101,
                   index % 7, index % 5, "null" if index % 4 else '"kill switch"'))
        flags.append(flag)
        total += len(flag) + 2
        index += 1
    return '{"flags": [\n' + ",\n".join(flags) + "\n]}\n"


def make_nesting(size):
    depth = size // 2
    return "(" * depth + ")" * depth


def make_sum(size):
    parts = []
    total = 0
    index = 1
    while total < size:
        part = str(index % 10)
        parts.append(part)
        total += 2
        index += 1
    return "+".join(parts)


def make_grammar(size):
    """A grammar of many rules (the grammar is the input that matters):
    rule n reads its number in letters, then rule n + 1 or the end."""
    rules = ["list: r0.\n"]
    total = len(rules[0])
    index = 0
    while total < size:
        rule = '-r%d: "%s", -",", r%d; "%s".\n' % (index, letters(index), index + 1,
                                                    letters(index))
        rules.append(rule)
        total += len(rule)
        index += 1
    rules.append('-r%d: "end".\n' % index)
    return "".join(rules)


# name, grammar file, input maker, the two input sizes in bytes (the
# second is four times the first), limits at the larger size: processor
# seconds, resident MB, resident MB for each further MB of input (None:
# no claim), and the most the time may grow from the smaller input to
# the larger (None: no linear claim).
BENCHMARKS = [
    ("log", "log.ixml", make_log, 250_000, 1_000_000, 0.20, 60, 20, 6.0),
    ("csv", "csv.ixml", make_csv, 250_000, 1_000_000, 0.40, 100, 60, 6.0),
    ("calc", "calc.ixml", make_calc, 250_000, 1_000_000, 0.60, 150, 100, 6.0),
    ("markdown", "markdown.ixml", make_markdown, 250_000, 1_000_000, 0.40, 120, 80, 6.0),
    ("right-recursion", "numbers.ixml", make_numbers, 250_000, 1_000_000, 0.40, 80, 40, 6.0),
    ("nesting", "nesting.ixml", make_nesting, 50_000, 200_000, 0.20, 100, None, 6.0),
    ("ambiguous", "sum.ixml", make_sum, 500, 2_000, 2.00, 200, None, None),
    ("grammar", None, make_grammar, 50_000, 200_000, 0.50, 200, None, 6.0),
    ("log-10mb", "log.ixml", make_log, 2_500_000, 10_000_000, 1.50, 250, 20, 6.0),
]

# The comparison with tree-sitter (the book's "Compared with tree-sitter"):
# the same input parsed by another parser in the same process, with the
# same limits as above. The query is an XPath expression of $input.
COMPARISONS = [
    ("json-ixml", "json.ixml", make_json, 250_000, 1_000_000, 0.30, 80, 60, 6.0),
    ("json-tree-sitter", 'tree-sitter:parse(unparsed-text($input), "json", {"highlights": false()})',
     make_json, 250_000, 1_000_000, 0.35, 90, 70, 6.0),
    ("json-parse-json", "json-to-xml(unparsed-text($input))",
     make_json, 250_000, 1_000_000, 0.25, 60, 40, 6.0),
    ("md-tree-sitter", 'tree-sitter:parse(unparsed-text($input), "markdown", {"highlights": false()})',
     make_markdown, 250_000, 1_000_000, 1.60, 160, 140, 6.0),
]

COMPARISON_QUERY = """declare variable $input external;
let $document := %s
return count($document//*)"""

# Run with --large only.
LARGE_BENCHMARKS = [
    ("log-100mb", "log.ixml", make_log, 25_000_000, 100_000_000, 15.0, 2000, 20, 6.0),
]


def run_once(command):
    """Runs command once: processor seconds, exit code, output, error and
    most resident memory in MB (from wait4's resource usage)."""
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as error:
        process = subprocess.Popen(command, stdout=output, stderr=error)
        _, status, usage = os.wait4(process.pid, 0)
        code = os.waitstatus_to_exitcode(status)
        output.seek(0)
        error.seek(0)
        return (usage.ru_utime + usage.ru_stime, code, output.read(), error.read(),
                usage.ru_maxrss / 1024.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build")
    parser.add_argument("--program")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--large", action="store_true")
    arguments = parser.parse_args()
    if arguments.program:
        program = os.path.abspath(arguments.program)
    elif arguments.build:
        program = os.path.join(os.path.abspath(arguments.build), "apps", "sxml")
    else:
        parser.error("--build or --program is required")
    work = tempfile.mkdtemp(prefix="sxml-ixml-bench-")
    failures = []
    print("%-16s %9s %9s %9s %9s  %s" % ("benchmark", "bytes", "cpu s", "max MB", "MB/s",
                                          "output"))
    try:
        benchmarks = (BENCHMARKS + COMPARISONS
                      + (LARGE_BENCHMARKS if arguments.large else []))
        for (name, grammar, maker, small, large, seconds, megabytes, per_megabyte,
             growth) in benchmarks:
            if arguments.only and name not in arguments.only:
                continue
            times = []
            residents = []
            for size in (small, large):
                text = maker(size)
                input_path = os.path.join(work, "%s-%d.txt" % (name, size))
                with open(input_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(text)
                if grammar is None:
                    # The grammar benchmark compiles the generated grammar
                    # and parses a short list with it.
                    grammar_path = input_path
                    input_path = os.path.join(work, "%s-%d.input" % (name, size))
                    with open(input_path, "w", encoding="utf-8") as handle:
                        handle.write("a,b,c")
                    query = GRAMMAR_QUERY
                elif not grammar.endswith(".ixml"):
                    # A comparison: another parser, given as an expression.
                    grammar_path = ""
                    query = COMPARISON_QUERY % grammar
                else:
                    grammar_path = os.path.join(GRAMMARS, grammar)
                    query = QUERY
                command = [program, "xquery", "--param=input=" + input_path,
                           "--expression=" + query]
                if grammar_path:
                    command.insert(2, "--param=grammar=" + grammar_path)
                best = None
                for _ in range(arguments.repeat):
                    result = run_once(command)
                    if best is None or result[0] < best[0]:
                        best = result
                elapsed, code, output, error, resident = best
                state = (output.decode(errors="replace").strip().replace("\n", " ")
                         if code == 0 else "failed (exit %d): %s" % (
                             code, error.decode(errors="replace").strip()[:160]))
                rate = len(text.encode("utf-8")) / 1e6 / elapsed if elapsed > 0 else 0.0
                print("%-16s %9d %9.3f %9.1f %9.2f  %s" % (name, len(text.encode("utf-8")),
                                                         elapsed, resident, rate, state))
                sys.stdout.flush()
                times.append(elapsed)
                residents.append(resident)
                if code != 0:
                    failures.append("%s at %d bytes failed" % (name, size))
                if size == large and elapsed > seconds:
                    failures.append("%s: %.3f s, more than %.2f s" % (name, elapsed, seconds))
                if size == large and resident > megabytes:
                    failures.append("%s: %.0f MB, more than %d MB" % (name, resident, megabytes))
            if per_megabyte is not None and len(residents) == 2:
                further = (residents[1] - residents[0]) / ((large - small) / 1e6)
                print("%-16s %9s %9s %9.1f  MB for each further MB" % (name, "", "", further))
                if further > per_megabyte:
                    failures.append("%s: %.1f MB for each further MB, more than %d"
                                    % (name, further, per_megabyte))
            # Below 20 ms the process start dominates: no growth to judge.
            if growth is not None and times[0] > 0.02 and times[1] / times[0] > growth:
                failures.append("%s: four times the input took %.1f times as long, more than %.1f"
                                % (name, times[1] / times[0], growth))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if arguments.check:
        for failure in failures:
            print("limit: " + failure)
        print("check: %s" % ("passed" if not failures else "%d failed" % len(failures)))
        return 1 if failures else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
