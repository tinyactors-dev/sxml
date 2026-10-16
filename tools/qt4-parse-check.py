#!/usr/bin/env python3
"""Parse every applicable QT4 test query with an sxml grammar and classify the results.

Usage: tools/qt4-parse-check.py [--spec XQ40] [--grammar xquery] [--report FILE]
                                [--suite DIRECTORY] [--build-directory DIRECTORY]

For each test case applicable to the spec (XP40 or XQ40; applicability as in
tools/check-test-list.py and docs/QT4_CATALOG.md), the query text (inline
<test> content or the file named by <test file="...">) is parsed with the
tree-sitter grammar grammars/<grammar>/ through tools/qt4-parse-harness.c.

Each test case falls into one of these classes:
  accepted          expected to parse, parsed cleanly
  rejected          expected XPST0003, parse failed
  false reject      expected to parse, but the parse failed
  false accept      expected XPST0003 only, but parsed cleanly (many of these are
                    detected later, in the tree-building step)
  either            the result allows XPST0003 among other outcomes (any-of)
Test cases needing XQuery Update, Full Text or XSLT syntax are counted
separately ("extension syntax"), as sxml does not implement those grammars.

The default grammar is xpath for XP40 and xquery for XQ40.
"""

import argparse
import collections
import os
import subprocess
import sys
import xml.etree.ElementTree as element_tree

CATALOG_NAMESPACE = "{http://www.w3.org/2010/09/qt-fots-catalog}"
EXTENSION_FEATURES = ("XQUpdate", "fullText", "fn-transform-XSLT")


def spec_token_matches(token, language, version):
    """Return True when a token such as XP31+ admits language/version."""
    or_later = token.endswith("+")
    token = token.rstrip("+")
    if token[:2] != language:
        return False
    token_version = int(token[2:])
    return token_version == version or (or_later and token_version <= version)


def dependencies(element, dependency_type):
    return [
        dependency.get("value", "")
        for dependency in element.findall(CATALOG_NAMESPACE + "dependency")
        if dependency.get("type") == dependency_type and dependency.get("satisfied", "true") == "true"
    ]


def expected_parse(result):
    """Return 'reject', 'either' or 'accept' for a <result> element."""
    assertions = list(result)
    if not assertions:
        return "accept"
    assertion = assertions[0]
    tag = assertion.tag.replace(CATALOG_NAMESPACE, "")
    if tag == "error":
        return "reject" if assertion.get("code") == "XPST0003" else "accept"
    if tag == "any-of":
        codes = [
            child.get("code")
            for child in assertion.iter(CATALOG_NAMESPACE + "error")
        ]
        if "XPST0003" in codes:
            return "either"
    return "accept"


def load_cases(suite_directory, spec):
    language = spec[:2]
    version = int(spec[2:])
    catalog = element_tree.parse(os.path.join(suite_directory, "catalog.xml"))
    cases = []
    for test_set_entry in catalog.getroot().findall(CATALOG_NAMESPACE + "test-set"):
        test_set_path = os.path.join(suite_directory, test_set_entry.get("file"))
        test_set = element_tree.parse(test_set_path).getroot()
        test_set_specs = dependencies(test_set, "spec")
        test_set_features = dependencies(test_set, "feature")
        for test_case in test_set.findall(CATALOG_NAMESPACE + "test-case"):
            specs = dependencies(test_case, "spec") or test_set_specs
            applicable = all(
                any(spec_token_matches(token, language, version) for token in value.split())
                for value in specs
            )
            if not applicable:
                continue
            test = test_case.find(CATALOG_NAMESPACE + "test")
            if test is None:
                continue
            if test.get("file"):
                query_path = os.path.join(os.path.dirname(test_set_path), test.get("file"))
                with open(query_path, "rb") as query_file:
                    query = query_file.read()
                if query.startswith(b"\xef\xbb\xbf"):
                    query = query[3:]
            else:
                query = (test.text or "").encode("utf-8")
            features = dependencies(test_case, "feature") + test_set_features
            result = test_case.find(CATALOG_NAMESPACE + "result")
            cases.append({
                "test_set": test_set_entry.get("name"),
                "name": test_case.get("name"),
                "query": query,
                "expected": expected_parse(result) if result is not None else "accept",
                "extension": any(feature.startswith(EXTENSION_FEATURES) for feature in features),
            })
    return cases


def build_harness(repository_root, grammar, build_directory):
    os.makedirs(build_directory, exist_ok=True)
    executable = os.path.join(build_directory, f"qt4-parse-harness-{grammar}")
    grammar_source = os.path.join(repository_root, "grammars", grammar, "src")
    tree_sitter = os.path.join(repository_root, "third_party", "tree-sitter", "lib")
    sources = [
        os.path.join(tree_sitter, "src", "lib.c"),
        os.path.join(grammar_source, "parser.c"),
        os.path.join(grammar_source, "scanner.c"),
        os.path.join(repository_root, "tools", "qt4-parse-harness.c"),
    ]
    newest_source = max(os.path.getmtime(source) for source in sources)
    if os.path.exists(executable) and os.path.getmtime(executable) >= newest_source:
        return executable
    compiler = os.environ.get("CC", "cc")
    command = [
        compiler, "-O2", "-std=c11", "-w", "-D_DEFAULT_SOURCE",
        f"-DSXML_PARSE_LANGUAGE=tree_sitter_{grammar}",
        "-I", os.path.join(tree_sitter, "include"),
        "-I", os.path.join(tree_sitter, "src"),
        "-I", grammar_source,
        *sources, "-o", executable,
    ]
    subprocess.run(command, check=True)
    return executable


def parse_all(executable, cases):
    payload = bytearray()
    for case in cases:
        payload += f"{len(case['query'])}\n".encode("ascii") + case["query"]
    completed = subprocess.run([executable], input=bytes(payload), capture_output=True, check=True)
    lines = completed.stdout.decode("utf-8").splitlines()
    if len(lines) != len(cases):
        sys.exit(f"harness returned {len(lines)} results for {len(cases)} queries")
    for case, line in zip(cases, lines):
        case["outcome"] = line


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec", default="XQ40", help="XP40 or XQ40")
    parser.add_argument("--grammar", help="grammar directory name under grammars/")
    parser.add_argument("--suite", help="QT4 test suite directory")
    parser.add_argument("--build-directory", help="where the harness is compiled")
    parser.add_argument("--report", help="write every false reject and false accept here")
    arguments = parser.parse_args()

    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    suite_directory = arguments.suite or os.path.join(repository_root, "third_party", "qt4tests")
    if not os.path.exists(os.path.join(suite_directory, "catalog.xml")):
        sys.exit(f"{suite_directory}: no catalog.xml; fetch the QT4 test suite first")
    grammar = arguments.grammar or ("xpath" if arguments.spec.startswith("XP") else "xquery")
    build_directory = arguments.build_directory or os.path.join(
        repository_root, "build", "qt4-parse-check"
    )

    cases = load_cases(suite_directory, arguments.spec)
    executable = build_harness(repository_root, grammar, build_directory)
    parse_all(executable, cases)

    counts = collections.Counter()
    by_test_set = collections.Counter()
    report_lines = []
    for case in cases:
        parsed = case["outcome"] == "ok"
        if case["extension"]:
            category = "extension syntax, parsed" if parsed else "extension syntax, rejected"
        elif case["expected"] == "either":
            category = "either"
        elif case["expected"] == "reject":
            category = "false accept" if parsed else "rejected"
        else:
            category = "accepted" if parsed else "false reject"
        counts[category] += 1
        if category in ("false reject", "false accept"):
            by_test_set[(category, case["test_set"])] += 1
            report_lines.append(
                f"{category}\t{case['test_set']}\t{case['name']}\t{case['outcome']}"
            )

    core_total = sum(
        count for category, count in counts.items() if not category.startswith("extension")
    )
    expected_accepts = counts["accepted"] + counts["false reject"]
    expected_rejects = counts["rejected"] + counts["false accept"]
    print(f"{arguments.spec} with grammars/{grammar}: {len(cases)} applicable test cases")
    for category in ("accepted", "false reject", "rejected", "false accept", "either",
                     "extension syntax, parsed", "extension syntax, rejected"):
        print(f"  {category:28} {counts[category]:6}")
    if expected_accepts:
        print(f"  false reject rate            {100.0 * counts['false reject'] / expected_accepts:6.2f}%"
              f" of {expected_accepts} valid queries")
    if expected_rejects:
        print(f"  false accept rate            {100.0 * counts['false accept'] / expected_rejects:6.2f}%"
              f" of {expected_rejects} XPST0003 queries")
    print(f"  (core total {core_total})")
    print("worst test sets:")
    for (category, test_set), count in by_test_set.most_common(15):
        print(f"  {count:5}  {category:13} {test_set}")
    if arguments.report:
        with open(arguments.report, "w", encoding="utf-8") as report:
            report.write("\n".join(sorted(report_lines)) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
