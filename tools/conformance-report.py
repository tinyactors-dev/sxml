#!/usr/bin/env python3
"""Turns sxml-qt4 run reports into docs/CONFORMANCE.md (packet 1.27).

Input is the directory tools/run-qt4.sh writes: one JSON report per test
set (the layout of tests/qt4/driver/report.h, version 1) under a directory
per specification (XP40, and XQ40 once XQuery runs), the torture slice's
report and the commit of the code under test. Output:

- the Markdown dashboard (--output, default docs/CONFORMANCE.md): the
  slice, totals, a breakdown by area (the test-set name prefix: fn, op,
  prod, map, array, ...), the most frequent failure causes and not-run
  reasons, the test sets with the most failures, and every test set;
- with --json-output, one merged JSON report of the whole run;
- exit status 1 when fewer slice test cases pass than the baseline
  (--slice-baseline, default tests/qt4/slice-baseline.txt) records, or
  the slice run did not finish.

The output depends only on the input, so regenerating from the same run
gives the same file. Outcomes and "passed" follow D92: passed = pass +
wrong-error (catalog-schema.xsd: a wrong error code counts as a pass).

Usage:
  tools/conformance-report.py RUN-DIRECTORY [--output FILE]
      [--json-output FILE] [--slice-baseline FILE]
"""

import argparse
import collections
import json
import pathlib
import re
import sys

OUTCOMES = ("pass", "wrong_error", "fail", "error", "not_run")
SPECS = ("XP40", "XQ40")
# Areas in the order of the table; a test set belongs to the area of its
# name's prefix up to the first hyphen, and to "other" when none matches.
AREAS = (
    ("fn", "Functions (F&O 4.0, fn:)"),
    ("op", "Operators (F&O 4.0, op:)"),
    ("prod", "Grammar productions (XPath/XQuery 4.0)"),
    ("map", "Map functions (map:)"),
    ("array", "Array functions (array:)"),
    ("math", "Math functions (math:)"),
    ("xs", "Constructor functions and types (xs:)"),
    ("misc", "Miscellaneous"),
    ("app", "Applications"),
    ("method", "Serialization methods"),
    ("upd", "XQuery Update"),
    ("ft", "XQuery Full Text"),
    ("bin", "EXPath Binary"),
    ("expath", "EXPath (other)"),
    ("other", "Other"),
)
AREA_NAMES = {prefix for prefix, _ in AREAS}
TOP_COUNT = 25
ERROR_CODE = re.compile(r"\berr:([A-Z]{4}[0-9]{4})\b")
FINISHED_EXIT_CODES = ("0", "1")


def read_text(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def read_run_information(run_directory):
    information = {"commit": "unknown", "date": "unknown"}
    for line in read_text(run_directory / "run.txt").splitlines():
        key, _, value = line.partition(" ")
        information[key] = value.strip()
    return information


def empty_counts():
    return {"total": 0, **{outcome: 0 for outcome in OUTCOMES}}


def add_counts(into, counts):
    for key in into:
        into[key] += counts.get(key, 0)


def passed(counts):
    return counts["pass"] + counts["wrong_error"]


def read_spec(spec_directory):
    """Reads the per-test-set reports of one specification, in catalog
    order (the .test-sets file run-qt4.sh writes next to the directory)."""
    names_file = spec_directory.parent / (spec_directory.name + ".test-sets")
    if names_file.exists():
        names = [line.strip() for line in read_text(names_file).splitlines()
                 if line.strip()]
    else:
        names = sorted(path.stem for path in spec_directory.glob("*.json"))
    test_sets = []
    tests = []
    unfinished = []
    for name in names:
        exit_code = read_text(spec_directory / (name + ".exit")).strip()
        report_text = read_text(spec_directory / (name + ".json"))
        report = None
        if exit_code in FINISHED_EXIT_CODES or exit_code == "":
            try:
                report = json.loads(report_text)
            except json.JSONDecodeError:
                report = None
        if report is None:
            unfinished.append({"name": name, "exit_code": exit_code or "?"})
            continue
        for entry in report["test_sets"]:
            test_sets.append({key: entry[key]
                              for key in ("name", "total", *OUTCOMES)})
        tests.extend(report["tests"])
    totals = empty_counts()
    for entry in test_sets:
        add_counts(totals, entry)
    return {"spec": spec_directory.name, "test_sets": test_sets,
            "tests": tests, "totals": totals, "unfinished": unfinished}


def read_slice(run_directory):
    exit_code = read_text(run_directory / "slice.exit").strip()
    try:
        report = json.loads(read_text(run_directory / "slice.json"))
    except json.JSONDecodeError:
        report = None
    if exit_code not in FINISHED_EXIT_CODES or report is None:
        return {"finished": False, "exit_code": exit_code or "?",
                "totals": empty_counts(), "tests": []}
    totals = empty_counts()
    add_counts(totals, report["totals"])
    return {"finished": True, "exit_code": exit_code, "totals": totals,
            "tests": report["tests"]}


def area_of(test_set_name):
    prefix = test_set_name.split("-", 1)[0]
    return prefix if prefix in AREA_NAMES else "other"


def failure_cause(reason):
    """Groups a fail or error reason: the error code and message when the
    query raised an error, else the assertion that did not hold."""
    assertion, _, rest = reason.partition(": ")
    if rest.startswith("got error "):
        return shorten(rest[len("got error "):])
    if assertion == "query":
        return shorten(rest)
    if rest.startswith("got "):
        return "wrong result (" + assertion + ")"
    return shorten(reason)


def wrong_error_cause(reason):
    """"error: expected X, got err:Y: message" becomes "got Y: message"."""
    _, _, got = reason.partition(", got ")
    return shorten(got) if got else shorten(reason)


def not_run_cause(reason):
    text = reason[len("skip: "):] if reason.startswith("skip: ") else reason
    if text.startswith("spec "):
        return "spec: not applicable to this language"
    return shorten(text)


def shorten(text, limit=100):
    text = " ".join(text.split())
    if text.startswith("err:"):
        text = text[len("err:"):]
    return text if len(text) <= limit else text[:limit - 1].rstrip() + "…"


def count_causes(tests, outcomes, classify):
    counter = collections.Counter(
        classify(test["reason"]) for test in tests
        if test["outcome"] in outcomes)
    return sorted(counter.items(), key=lambda item: (-item[1], item[0]))


def count_error_codes(tests):
    counter = collections.Counter()
    for test in tests:
        if test["outcome"] not in ("fail", "error"):
            continue
        match = ERROR_CODE.search(test["reason"])
        if match:
            counter[match.group(1)] += 1
        elif test["reason"].startswith("query: SXML_STATUS_"):
            counter[test["reason"].split()[1].rstrip(":")] += 1
        else:
            counter["(no error: wrong result)"] += 1
    return sorted(counter.items(), key=lambda item: (-item[1], item[0]))


def markdown_cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def percent(part, whole):
    return "–" if whole == 0 else f"{100.0 * part / whole:.1f} %"


def counts_row(label, counts):
    run = counts["total"] - counts["not_run"]
    return (f"| {label} | {counts['total']:,} | {run:,} | {passed(counts):,} "
            f"| {counts['wrong_error']:,} | {counts['fail']:,} "
            f"| {counts['error']:,} | {counts['not_run']:,} "
            f"| {percent(passed(counts), run)} |")


COUNTS_HEADER = (
    "| {0} | Test cases | Run | Passed | of which wrong error | Failed "
    "| Errors | Not run | Passed of run |\n"
    "|---|--:|--:|--:|--:|--:|--:|--:|--:|")


def render_spec(lines, result):
    spec = result["spec"]
    totals = result["totals"]
    lines += [f"## {spec}", "",
              f"{len(result['test_sets'])} test sets with test cases"
              + (f"; {len(result['unfinished'])} did not finish"
                 if result["unfinished"] else ", all finished") + ".", "",
              COUNTS_HEADER.format("Specification"),
              counts_row(spec, totals), ""]

    lines += [f"### {spec} by area", "",
              COUNTS_HEADER.format("Area (test-set prefix)")]
    by_area = collections.OrderedDict(
        (prefix, empty_counts()) for prefix, _ in AREAS)
    for entry in result["test_sets"]:
        add_counts(by_area[area_of(entry["name"])], entry)
    for prefix, title in AREAS:
        if by_area[prefix]["total"]:
            label = f"{title}: `{prefix}-*`" if prefix != "other" else title
            lines.append(counts_row(label, by_area[prefix]))
    lines.append("")

    tests = result["tests"]
    lines += [f"### {spec} failure causes", "",
              "Failed and error test cases by the error code the query "
              "raised (or the status the driver got):", "",
              "| Error code | Test cases |", "|---|--:|"]
    for code, count in count_error_codes(tests)[:TOP_COUNT]:
        lines.append(f"| {markdown_cell(code)} | {count:,} |")
    lines += ["", f"The {TOP_COUNT} most frequent reasons:", "",
              "| Cause | Test cases |", "|---|--:|"]
    for cause, count in count_causes(tests, ("fail", "error"),
                                     failure_cause)[:TOP_COUNT]:
        lines.append(f"| {markdown_cell(cause)} | {count:,} |")
    lines += ["", "Wrong errors (counted as passed) by the error raised:", "",
              "| Raised instead | Test cases |", "|---|--:|"]
    for cause, count in count_causes(tests, ("wrong-error",),
                                     wrong_error_cause)[:10]:
        lines.append(f"| {markdown_cell(cause)} | {count:,} |")
    lines += ["", "Not run, by the first dependency that is not met:", "",
              "| Reason | Test cases |", "|---|--:|"]
    for cause, count in count_causes(tests, ("not-run",),
                                     not_run_cause)[:15]:
        lines.append(f"| {markdown_cell(cause)} | {count:,} |")
    lines.append("")

    lines += [f"### {spec} test sets with the most failures", "",
              "| Test set | Test cases | Passed | Failed | Errors | Not run |",
              "|---|--:|--:|--:|--:|--:|"]
    worst = sorted((entry for entry in result["test_sets"]
                    if entry["fail"] + entry["error"]),
                   key=lambda entry: (-(entry["fail"] + entry["error"]),
                                      entry["name"]))
    for entry in worst[:TOP_COUNT]:
        lines.append(f"| {entry['name']} | {entry['total']:,} "
                     f"| {passed(entry):,} | {entry['fail']:,} "
                     f"| {entry['error']:,} | {entry['not_run']:,} |")
    lines.append("")

    if result["unfinished"]:
        lines += [f"### {spec} test sets that did not finish", "",
                  "A crash, sanitizer report or timeout ended the process "
                  "(tools/run-qt4.sh runs one per test set).", "",
                  "| Test set | Exit code |", "|---|--:|"]
        for entry in result["unfinished"]:
            lines.append(f"| {entry['name']} | {entry['exit_code']} |")
        lines.append("")

    lines += [f"### Every {spec} test set", "", "<details>",
              f"<summary>{len(result['test_sets'])} test sets</summary>", "",
              "| Test set | Test cases | Passed | of which wrong error "
              "| Failed | Errors | Not run |",
              "|---|--:|--:|--:|--:|--:|--:|"]
    for entry in result["test_sets"]:
        lines.append(f"| {entry['name']} | {entry['total']:,} "
                     f"| {passed(entry):,} | {entry['wrong_error']:,} "
                     f"| {entry['fail']:,} | {entry['error']:,} "
                     f"| {entry['not_run']:,} |")
    lines += ["", "</details>", ""]


def render(information, slice_result, baseline, spec_results):
    commit = information["commit"]
    lines = [
        "# Conformance",
        "",
        "<!-- Generated by tools/conformance-report.py from a run of "
        "tools/run-qt4.sh; do not edit by hand. -->",
        "",
        "Results of the W3C QT4 test suite (`third_party/qt4tests`, D13) "
        "under `sxml-qt4 run` (D92), a release build, one process per test "
        "set. *Passed* counts passes and wrong errors (a wrong error code "
        "counts as a pass, catalog-schema.xsd); *not run* means not "
        "applicable to sxml or using an unsupported feature; *run* is "
        "test cases minus not run.",
        "",
        f"- Code under test: commit `{commit[:12]}` of {information['date']}",
        "- Regenerate: `cmake --preset release && cmake --build --preset "
        "release && tools/run-qt4.sh --program "
        "build/release/tests/qt4/driver/sxml-qt4 && "
        "tools/conformance-report.py build/qt4-run`",
        "",
        "## Torture slice",
        "",
    ]
    slice_totals = slice_result["totals"]
    if slice_result["finished"]:
        lines.append(f"**{passed(slice_totals)}/{slice_totals['total']} "
                     f"passed** (baseline {baseline}); "
                     f"{slice_totals['wrong_error']} wrong error, "
                     f"{slice_totals['fail']} failed, "
                     f"{slice_totals['error']} errors, "
                     f"{slice_totals['not_run']} not run "
                     "(`tests/qt4/torture-slice.txt`).")
        failing = [test for test in slice_result["tests"]
                   if test["outcome"] not in ("pass", "wrong-error")]
        if failing:
            lines += ["", "| Test set | Test case | Outcome | Reason |",
                      "|---|---|---|---|"]
            for test in failing:
                lines.append(f"| {test['test_set']} | {test['test_case']} "
                             f"| {test['outcome']} "
                             f"| {markdown_cell(shorten(test['reason']))} |")
    else:
        lines.append(f"**The slice run did not finish** (exit code "
                     f"{slice_result['exit_code']}).")
    lines.append("")
    for result in spec_results:
        render_spec(lines, result)
    return "\n".join(lines).rstrip("\n") + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="Turn sxml-qt4 run reports into docs/CONFORMANCE.md.")
    parser.add_argument("run_directory", type=pathlib.Path,
                        help="the --output directory of tools/run-qt4.sh")
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent
                        / "docs" / "CONFORMANCE.md")
    parser.add_argument("--json-output", type=pathlib.Path)
    parser.add_argument("--slice-baseline", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent
                        / "tests" / "qt4" / "slice-baseline.txt")
    arguments = parser.parse_args()

    run_directory = arguments.run_directory
    information = read_run_information(run_directory)
    slice_result = read_slice(run_directory)
    spec_results = [read_spec(run_directory / spec) for spec in SPECS
                    if (run_directory / spec).is_dir()]
    if not spec_results:
        print(f"{run_directory}: no XP40 or XQ40 reports", file=sys.stderr)
        return 2
    baseline = int(arguments.slice_baseline.read_text().strip())

    arguments.output.write_text(
        render(information, slice_result, baseline, spec_results),
        encoding="utf-8")
    if arguments.json_output:
        merged = {"report": "sxml conformance", "version": 1,
                  "commit": information["commit"],
                  "date": information["date"],
                  "slice": slice_result, "specs": spec_results}
        arguments.json_output.write_text(json.dumps(merged, indent=1) + "\n",
                                         encoding="utf-8")

    slice_passed = passed(slice_result["totals"])
    for result in spec_results:
        totals = result["totals"]
        print(f"{result['spec']}: {totals['total']} test cases: "
              f"{passed(totals)} passed ({totals['wrong_error']} wrong error), "
              f"{totals['fail']} failed, {totals['error']} errors, "
              f"{totals['not_run']} not run; "
              f"{len(result['unfinished'])} test sets did not finish")
    print(f"slice: {slice_passed}/{slice_result['totals']['total']} passed")
    if not slice_result["finished"]:
        print("the slice run did not finish", file=sys.stderr)
        return 1
    if slice_passed < baseline:
        print(f"{slice_passed} slice test cases passed, fewer than the "
              f"baseline {baseline}", file=sys.stderr)
        return 1
    if slice_passed > baseline:
        print(f"{slice_passed} slice test cases passed: raise the "
              f"baseline {baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
