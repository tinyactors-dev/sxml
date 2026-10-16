#!/usr/bin/env python3
"""Parse every applicable QT4 test query with an sxml grammar and classify the results.

Usage: tools/qt4-parse-check.py [--spec XQ40] [--grammar xquery] [--report FILE]
                                [--suite DIRECTORY] [--build-directory DIRECTORY]

With --spec XT40, the patterns in the XSLT 4.0 test suite (third_party/xslt40-test)
are parsed with grammars/xslt_pattern instead: every match, count, from,
group-starting-with and group-ending-with attribute of every stylesheet. A
pattern from a stylesheet that some test case expects to fail with XTSE0340
counts as "either"; patterns using syntax that the XSLT 4.0 draft has dropped
(type patterns "~T" and "type(T)", record patterns with field declarations)
are counted as "dropped syntax".

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
For XQ40, the library modules named by <module file="..."> are parsed too.
Test cases needing XQuery Update, Full Text or XSLT syntax are counted
separately ("extension syntax"), as sxml does not implement those grammars;
so are test cases that need XML 1.1 or an XML 1.0 edition before the fifth.

The default grammar is xpath for XP40 and xquery for XQ40.
"""

import argparse
import collections
import os
import re
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
    module_paths = {}
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
            if language == "XQ":
                for module in test_case.findall(CATALOG_NAMESPACE + "module"):
                    module_path = os.path.normpath(
                        os.path.join(os.path.dirname(test_set_path), module.get("file"))
                    )
                    module_paths.setdefault(module_path, test_set_entry.get("name"))
            features = dependencies(test_case, "feature") + test_set_features
            xml_versions = dependencies(test_case, "xml-version") or dependencies(test_set, "xml-version")
            result = test_case.find(CATALOG_NAMESPACE + "result")
            cases.append({
                "test_set": test_set_entry.get("name"),
                "name": test_case.get("name"),
                "query": query,
                "expected": expected_parse(result) if result is not None else "accept",
                "extension": any(feature.startswith(EXTENSION_FEATURES) for feature in features),
                # sxml follows XML 1.0 Fifth Edition (xgc: xml-version).
                "other_xml_version": any(
                    not any(token in ("1.0", "1.0:5+") for token in value.split())
                    for value in xml_versions
                ),
            })
    # Library modules imported by XQuery test cases must parse as well. A few
    # are deliberately broken; their importing test cases expect an error,
    # so they are reported in their own category.
    for module_path, test_set_name in sorted(module_paths.items()):
        if not os.path.exists(module_path):
            continue
        with open(module_path, "rb") as module_file:
            query = module_file.read()
        if query.startswith(b"\xef\xbb\xbf"):
            query = query[3:]
        cases.append({
            "test_set": test_set_name,
            "name": os.path.relpath(module_path, suite_directory),
            "query": query,
            "expected": "module",
            "extension": False,
            "other_xml_version": False,
        })
    return cases


XSLT_CATALOG_NAMESPACE = "{http://www.w3.org/2012/10/xslt-test-catalog}"
XSLT_NAMESPACE = "{http://www.w3.org/1999/XSL/Transform}"
PATTERN_ATTRIBUTES = {
    "template": ("match",),
    "key": ("match",),
    "accumulator-rule": ("match",),
    "number": ("count", "from"),
    "for-each-group": ("group-starting-with", "group-ending-with"),
}


def load_pattern_cases(suite_directory):
    """Collect XSLT patterns from every stylesheet of the XSLT 4.0 test suite."""
    error_stylesheets = set()
    catalog = element_tree.parse(os.path.join(suite_directory, "catalog.xml"))
    for test_set_entry in catalog.getroot().findall(XSLT_CATALOG_NAMESPACE + "test-set"):
        test_set_path = os.path.join(suite_directory, test_set_entry.get("file"))
        test_set = element_tree.parse(test_set_path).getroot()
        for test_case in test_set.findall(XSLT_CATALOG_NAMESPACE + "test-case"):
            result = test_case.find(XSLT_CATALOG_NAMESPACE + "result")
            codes = [] if result is None else [
                error.get("code") for error in result.iter(XSLT_CATALOG_NAMESPACE + "error")
            ]
            if "XTSE0340" not in codes:
                continue
            for stylesheet in test_case.iter(XSLT_CATALOG_NAMESPACE + "stylesheet"):
                if stylesheet.get("file"):
                    error_stylesheets.add(os.path.normpath(
                        os.path.join(os.path.dirname(test_set_path), stylesheet.get("file"))
                    ))
    cases = []
    tests_directory = os.path.join(suite_directory, "tests")
    for directory, _, file_names in sorted(os.walk(tests_directory)):
        for file_name in sorted(file_names):
            if not file_name.endswith((".xsl", ".xslt")):
                continue
            stylesheet_path = os.path.normpath(os.path.join(directory, file_name))
            try:
                elements = list(element_tree.parse(stylesheet_path).iter())
            except element_tree.ParseError:
                continue
            for element in elements:
                if not isinstance(element.tag, str) or not element.tag.startswith(XSLT_NAMESPACE):
                    continue
                local_name = element.tag[len(XSLT_NAMESPACE):]
                for attribute in PATTERN_ATTRIBUTES.get(local_name, ()):
                    pattern = element.get(attribute)
                    if pattern is None:
                        continue
                    stripped = pattern.strip()
                    cases.append({
                        "test_set": os.path.relpath(directory, tests_directory),
                        "name": f"{file_name} {local_name}/@{attribute}: {pattern[:80]}",
                        "query": pattern.encode("utf-8"),
                        "expected": "either" if stylesheet_path in error_stylesheets else "accept",
                        "extension": "~" in stripped or stripped.startswith("type(")
                        or re.search(r"record\([^)]* as ", stripped) is not None,
                        "other_xml_version": False,
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
    parser.add_argument("--spec", default="XQ40", help="XP40, XQ40, or XT40 for XSLT patterns")
    parser.add_argument("--grammar", help="grammar directory name under grammars/")
    parser.add_argument("--suite", help="QT4 test suite directory")
    parser.add_argument("--build-directory", help="where the harness is compiled")
    parser.add_argument("--report", help="write every false reject and false accept here")
    arguments = parser.parse_args()

    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    suite_directory = arguments.suite or os.path.join(repository_root, "third_party", "qt4tests")
    is_pattern_check = arguments.spec == "XT40"
    if is_pattern_check and not arguments.suite:
        suite_directory = os.path.join(repository_root, "third_party", "xslt40-test")
    default_grammar = {"XP": "xpath", "XQ": "xquery", "XT": "xslt_pattern"}[arguments.spec[:2]]
    grammar = arguments.grammar or default_grammar
    build_directory = arguments.build_directory or os.path.join(
        repository_root, "build", "qt4-parse-check"
    )

    if not os.path.exists(os.path.join(suite_directory, "catalog.xml")):
        sys.exit(f"{suite_directory}: no catalog.xml; run tools/update-test-suites.sh")
    if is_pattern_check:
        cases = load_pattern_cases(suite_directory)
    else:
        cases = load_cases(suite_directory, arguments.spec)
    executable = build_harness(repository_root, grammar, build_directory)
    parse_all(executable, cases)

    counts = collections.Counter()
    by_test_set = collections.Counter()
    report_lines = []
    for case in cases:
        parsed = case["outcome"] == "ok"
        if case["extension"] and is_pattern_check:
            category = "dropped syntax, parsed" if parsed else "dropped syntax, rejected"
        elif case["other_xml_version"]:
            category = "other XML version, parsed" if parsed else "other XML version, rejected"
        elif case["extension"]:
            category = "extension syntax, parsed" if parsed else "extension syntax, rejected"
        elif case["expected"] == "module":
            category = "library module, parsed" if parsed else "library module, rejected"
            if not parsed:
                report_lines.append(
                    f"{category}\t{case['test_set']}\t{case['name']}\t{case['outcome']}"
                )
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
        count for category, count in counts.items()
        if not category.startswith(("extension", "other XML version", "library module", "dropped"))
    )
    expected_accepts = counts["accepted"] + counts["false reject"]
    expected_rejects = counts["rejected"] + counts["false accept"]
    unit = "patterns" if is_pattern_check else "test cases"
    print(f"{arguments.spec} with grammars/{grammar}: {len(cases)} applicable {unit}")
    for category in ("accepted", "false reject", "rejected", "false accept", "either",
                     "extension syntax, parsed", "extension syntax, rejected",
                     "other XML version, parsed", "other XML version, rejected",
                     "library module, parsed", "library module, rejected",
                     "dropped syntax, parsed", "dropped syntax, rejected"):
        if category.startswith(("dropped", "library", "other XML", "extension")) and not counts[category]:
            continue
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
