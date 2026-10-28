#!/usr/bin/env python3
"""Runs the evaluator benchmarks (packet perf-1) and prints their times.

Usage: tests/bench/run-bench.py --build DIR [--repeat N] [--only NAME]...
                               [--outputs DIR]

  --build DIR    a build directory (the release preset: build/release),
                 with apps/sxml and tests/qt4/driver/sxml-qt4
  --repeat N     runs of each benchmark; the fastest is reported (default 3)
  --only NAME    run only the benchmarks named (repeatable)
  --outputs DIR  write each benchmark's standard output to DIR/NAME.out, so
                 two builds can be compared byte by byte

Each benchmark is one process. The XQuery and XSLT ones run the sxml
command over the XMark auction document of the QT4 suite
(third_party/qt4tests/app/XMark/XMarkAuction.xml, 3.5 MB, made by the
XMark generator); "itunes" runs QT4's app-Demos itunes through sxml-qt4
(10 MB, schema-validated). The memory is the most resident memory of the
process. A run that does not exit with 0 is
reported as failed. Times are processor seconds (user plus system, from
wait4), the fastest of the runs: wall-clock time is too noisy on a machine
that other builds share.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(BENCH))
AUCTION = os.path.join(ROOT, "third_party/qt4tests/app/XMark/XMarkAuction.xml")
XMARK_ALL = os.path.join(ROOT, "third_party/qt4tests/app/XMark/XMark-All.xq")


def xquery(query):
    return ["SXML", "xquery", "--context-item=" + AUCTION, query]


def xslt(stylesheet):
    return ["SXML", "transform", stylesheet, AUCTION]


BENCHMARKS = [
    # Only parses the document: the part of every XMark benchmark that is
    # not evaluation.
    ("parse", ["SXML", "xquery", "--context-item=" + AUCTION, "--expression=count(/)"]),
    ("xmark-all", xquery(XMARK_ALL)),
    ("xmark-q8", xquery(os.path.join(BENCH, "xmark-q8.xq"))),
    ("xmark-q9", xquery(os.path.join(BENCH, "xmark-q9.xq"))),
    ("xmark-q11", xquery(os.path.join(BENCH, "xmark-q11.xq"))),
    ("predicate-join", xquery(os.path.join(BENCH, "predicate-join.xq"))),
    ("numeric-join", xquery(os.path.join(BENCH, "numeric-join.xq"))),
    ("descendant-join", xquery(os.path.join(BENCH, "descendant-join.xq"))),
    ("paths", xquery(os.path.join(BENCH, "paths.xq"))),
    ("recursion", xquery(os.path.join(BENCH, "recursion.xq"))),
    ("recursion-tail", xquery(os.path.join(BENCH, "recursion-tail.xq"))),
    # The regex benchmarks read no document (D308, D310).
    ("regex-reuse", ["SXML", "xquery", os.path.join(BENCH, "regex-reuse.xq")]),
    ("regex-match", ["SXML", "xquery", os.path.join(BENCH, "regex-match.xq")]),
    ("regex-literal", ["SXML", "xquery", os.path.join(BENCH, "regex-literal.xq")]),
    ("coercion", ["SXML", "xquery", os.path.join(BENCH, "coercion.xq")]),
    ("flwor-group", xquery(os.path.join(BENCH, "flwor-group.xq"))),
    ("flwor-order", xquery(os.path.join(BENCH, "flwor-order.xq"))),
    ("xslt-key", xslt(os.path.join(BENCH, "xslt-key.xsl"))),
    ("xslt-match-variable", xslt(os.path.join(BENCH, "xslt-match-variable.xsl"))),
    ("xslt-many-rules", xslt(os.path.join(BENCH, "xslt-many-rules.xsl"))),
    ("itunes", ["QT4", "run", "--spec=XQ40", os.path.join(BENCH, "itunes.txt")]),
]


def run_once(command):
    """Runs command once: its processor seconds (user and system), exit code, output, error and most
    resident memory in MB (from wait4's resource usage)."""
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as error:
        process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=error)
        _, status, usage = os.wait4(process.pid, 0)
        elapsed = usage.ru_utime + usage.ru_stime
        process.returncode = os.waitstatus_to_exitcode(status)
        output.seek(0)
        error.seek(0)
        return (elapsed, process.returncode, output.read(), error.read(),
                usage.ru_maxrss / 1024.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--outputs")
    arguments = parser.parse_args()
    programs = {
        "SXML": os.path.join(arguments.build, "apps/sxml"),
        "QT4": os.path.join(arguments.build, "tests/qt4/driver/sxml-qt4"),
    }
    if arguments.outputs:
        os.makedirs(arguments.outputs, exist_ok=True)
    print("%-22s %9s %9s  %s" % ("benchmark", "cpu s", "max MB", "output"))
    for name, template in BENCHMARKS:
        if arguments.only and name not in arguments.only:
            continue
        command = [programs.get(part, part) for part in template]
        best = None
        for _ in range(arguments.repeat):
            result = run_once(command)
            if best is None or result[0] < best[0]:
                best = result
        elapsed, code, output, error, resident = best
        digest = hashlib.sha256(output).hexdigest()[:12]
        state = digest if code == 0 else "failed (exit %d): %s" % (
            code, error.decode(errors="replace").strip()[:200])
        print("%-22s %9.3f %9.1f  %s" % (name, elapsed, resident, state))
        sys.stdout.flush()
        if arguments.outputs:
            with open(os.path.join(arguments.outputs, name + ".out"), "wb") as handle:
                handle.write(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
