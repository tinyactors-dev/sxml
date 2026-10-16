#!/usr/bin/env python3
"""Check a QT4 test list file against the fetched QT4 test suite.

Usage: tools/check-test-list.py LIST_FILE [SUITE_DIRECTORY] [--spec XP40]

Each non-comment line of LIST_FILE is `<test-set-name> <test-case-name>`,
optionally followed by `# reason`. The script reports, and exits non-zero for:
  - unknown test sets or test cases,
  - duplicate lines,
  - test cases not applicable to the requested spec (default XP40).
Spec applicability follows docs/QT4_CATALOG.md: a test-case `spec` dependency
replaces the test-set one; no `spec` dependency means every spec.
SUITE_DIRECTORY defaults to third_party/qt4tests (vendored, see tools/update-test-suites.sh).
"""

import argparse
import os
import sys
import xml.etree.ElementTree as element_tree

CATALOG_NAMESPACE = "{http://www.w3.org/2010/09/qt-fots-catalog}"


def spec_token_matches(token, language, version):
    """Return True when a token such as XP31+ admits language/version."""
    or_later = token.endswith("+")
    token = token.rstrip("+")
    if token[:2] != language:
        return False
    token_version = int(token[2:])
    return token_version == version or (or_later and token_version <= version)


def spec_values(element):
    return [
        dependency.get("value", "")
        for dependency in element.findall(CATALOG_NAMESPACE + "dependency")
        if dependency.get("type") == "spec"
    ]


def load_suite(suite_directory):
    """Map test-set name -> {test-case name: effective spec values}."""
    catalog = element_tree.parse(os.path.join(suite_directory, "catalog.xml"))
    suite = {}
    for test_set_entry in catalog.getroot().findall(CATALOG_NAMESPACE + "test-set"):
        test_set_path = os.path.join(suite_directory, test_set_entry.get("file"))
        test_set = element_tree.parse(test_set_path).getroot()
        test_set_specs = spec_values(test_set)
        cases = {}
        for test_case in test_set.findall(CATALOG_NAMESPACE + "test-case"):
            cases[test_case.get("name")] = spec_values(test_case) or test_set_specs
        suite[test_set_entry.get("name")] = cases
    return suite


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("list_file")
    parser.add_argument("suite_directory", nargs="?")
    parser.add_argument("--spec", default="XP40", help="for example XP40 or XQ40")
    arguments = parser.parse_args()

    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    suite_directory = arguments.suite_directory or os.path.join(
        repository_root, "third_party", "qt4tests"
    )
    if not os.path.exists(os.path.join(suite_directory, "catalog.xml")):
        sys.exit(f"{suite_directory}: no catalog.xml; see tools/update-test-suites.sh")
    language = arguments.spec[:2]
    version = int(arguments.spec[2:])

    suite = load_suite(suite_directory)
    problems = []
    seen = set()
    entry_count = 0
    with open(arguments.list_file, encoding="utf-8") as list_file:
        for line_number, line in enumerate(list_file, start=1):
            content = line.split("#", 1)[0].split()
            if not content:
                continue
            location = f"{arguments.list_file}:{line_number}"
            if len(content) != 2:
                problems.append(f"{location}: expected '<test-set> <test-case>'")
                continue
            entry_count += 1
            test_set_name, test_case_name = content
            if (test_set_name, test_case_name) in seen:
                problems.append(f"{location}: duplicate {test_set_name} {test_case_name}")
            seen.add((test_set_name, test_case_name))
            if test_set_name not in suite:
                problems.append(f"{location}: unknown test set {test_set_name}")
                continue
            if test_case_name not in suite[test_set_name]:
                problems.append(f"{location}: unknown test case {test_case_name}")
                continue
            specs = suite[test_set_name][test_case_name]
            applicable = all(
                any(spec_token_matches(token, language, version) for token in value.split())
                for value in specs
            )
            if not applicable:
                problems.append(
                    f"{location}: {test_case_name} not applicable to {arguments.spec} "
                    f"(spec {' / '.join(specs)})"
                )

    for problem in problems:
        print(problem)
    print(f"{entry_count} entries checked, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
