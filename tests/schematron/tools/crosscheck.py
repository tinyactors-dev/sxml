#!/usr/bin/env python3
"""Run the suite's cases through SchXslt and compare with the expected results.

Usage:
  crosscheck.py --sxml PATH-TO-sxml --schxslt PATH-TO/pipeline-for-svrl.xsl
                [--editions 2020] [--set NAME]... [--case NAME]... [--report FILE]

SchXslt (an XSLT implementation of Schematron, MIT licence) compiles each
schema into an XSLT stylesheet, which then validates the instance and writes
SVRL. Both steps run with the `sxml transform` command. The SVRL is turned
into the suite's neutral findings (kind, location, id, role, flag, severity,
document, message, diagnostics, properties) and flags, and compared with the
case's expected result:

- findings: the multisets must be equal; a missing location in the expected
  finding is not compared; the message, diagnostic and property texts are
  compared after whitespace normalization (5.3);
- schema-error, error, any-of of these: the compile or the validation must
  fail (SchXslt does not tell a schema error from a dynamic one);
- implementation-defined: nothing is compared, the outcome is recorded;
- assert-svrl: each expression must be true of the SVRL document.

By default only the cases with since="2020" run: SchXslt 1.10.1 implements
the third edition (ISO/IEC 19757-3:2020). A case that depends on a query
binding SchXslt does not support is recorded as such.
"""
import argparse
import collections
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree

SUITE = "urn:sxml:schematron-test-suite"
SVRL = "http://purl.oclc.org/dsdl/svrl"
Q = "{%s}" % SUITE
V = "{%s}" % SVRL
XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"
SCHXSLT_BINDINGS = {"xslt2", "xslt3"}  # SchXslt 1.10.1 rejects the xpath bindings (E0002)
TIMEOUT = 120


def normalize(text):
    return " ".join((text or "").split())


def run(command, output_file=None):
    try:
        completed = subprocess.run(command, capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, b"", b"timeout"
    if output_file and completed.returncode == 0:
        with open(output_file, "wb") as handle:
            handle.write(completed.stdout)
    return completed.returncode, completed.stdout, completed.stderr


def finding_key(kind, location, attributes, message, diagnostics, properties):
    return (kind, location,
            tuple(sorted((k, v) for k, v in attributes.items() if v is not None)),
            normalize(message), tuple(sorted(diagnostics)), tuple(sorted(properties)))


def expected_findings(result, compare_lang):
    findings = []
    for finding in result.iter(Q + "finding"):
        attributes = {name: finding.get(name) for name in ("id", "role", "flag", "severity", "document")}
        diagnostics = []
        for diagnostic in finding.findall(Q + "diagnostic"):
            lang = diagnostic.get(XML_LANG) if compare_lang else None
            diagnostics.append((diagnostic.get("ref"), diagnostic.get("role"), lang,
                                normalize(diagnostic.text)))
        properties = [(p.get("ref"), p.get("role"), p.get("scheme"), normalize(p.text))
                      for p in finding.findall(Q + "property")]
        findings.append(finding_key(finding.get("kind"), finding.get("location"), attributes,
                                    finding.findtext(Q + "message"), diagnostics, properties))
    return findings


def text_of(element):
    return normalize("".join(element.itertext())) if element is not None else ""


def local_path(uri):
    return uri[len("file://"):] if uri.startswith("file://") else uri


def root_name(path):
    try:
        return ElementTree.parse(path).getroot().tag.split("}")[-1]
    except (OSError, ElementTree.ParseError):
        return None


def actual_from_svrl(svrl_file, set_directory, instance_file):
    """Findings and flags from SchXslt's SVRL.

    SchXslt names the documents of a pattern in active-pattern/@documents and
    writes no fired-rule/@document; a finding is attributed to the one
    document listed, or, with several, to the one whose document element
    is the first step of the finding's location.
    """
    root = ElementTree.parse(svrl_file).getroot()
    findings, flags = [], set()
    documents = []
    for element in root:
        if element.tag == V + "active-pattern":
            documents = [local_path(uri) for uri in (element.get("documents") or "").split()]
        elif element.tag == V + "fired-rule":
            flags.update((element.get("flag") or "").split())
            if element.get("document"):
                documents = [local_path(element.get("document"))]
        elif element.tag in (V + "failed-assert", V + "successful-report"):
            kind = element.tag[len(V):]
            flags.update((element.get("flag") or "").split())
            location = element.get("location")
            document = documents[0] if len(documents) == 1 else None
            if len(documents) > 1 and location:
                first = location.split("/")[1].split("}")[-1].split("[")[0]
                matching = [d for d in documents if root_name(d) == first]
                document = matching[0] if len(matching) == 1 else None
            relative = None
            if document and os.path.abspath(document) != os.path.abspath(instance_file):
                relative = os.path.relpath(document, set_directory)
            attributes = {"id": element.get("id"), "role": element.get("role"),
                          "flag": element.get("flag"), "severity": element.get("severity"),
                          "document": relative}
            diagnostics = [(d.get("diagnostic"), d.get("role"), d.get(XML_LANG),
                            text_of(d.find(V + "text")))
                           for d in element.findall(V + "diagnostic-reference")]
            properties = [(p.get("property"), p.get("role"), p.get("scheme"),
                           text_of(p.find(V + "text")))
                          for p in element.findall(V + "property-reference")]
            findings.append((kind, location, attributes,
                             text_of(element.find(V + "text")), diagnostics, properties))
    return findings, flags


def compare_findings(expected, actual, compare_lang):
    actual_keys = []
    for kind, location, attributes, message, diagnostics, properties in actual:
        if not compare_lang:
            diagnostics = [(d[0], d[1], None, d[3]) for d in diagnostics]
        actual_keys.append(finding_key(kind, location, attributes, message, diagnostics, properties))
    unmatched = list(actual_keys)
    missing = []
    for key in expected:
        match = None
        for candidate in unmatched:
            if key[1] is None:
                if (key[0],) + key[2:] == (candidate[0],) + candidate[2:]:
                    match = candidate
                    break
            elif key == candidate:
                match = candidate
                break
        if match is None:
            missing.append(key)
        else:
            unmatched.remove(match)
    return missing, unmatched


def svrl_assertions_hold(sxml, svrl_file, expressions):
    failed = []
    for expression in expressions:
        query = ('declare namespace svrl = "%s"; boolean(%s)' % (SVRL, expression))
        code, out, err = run([sxml, "xquery", "--context-item=" + svrl_file,
                              "--method=text", "--expression=" + query])
        if code != 0 or out.decode().strip() != "true":
            failed.append(expression)
    return failed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sxml", required=True)
    parser.add_argument("--schxslt", required=True)
    parser.add_argument("--suite", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--editions", default="2020")
    parser.add_argument("--set", action="append", default=[])
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--report")
    arguments = parser.parse_args()
    editions = set(arguments.editions.split(","))
    suite = os.path.abspath(arguments.suite)
    catalog = ElementTree.parse(os.path.join(suite, "catalog.xml")).getroot()
    counts = collections.Counter()
    lines = []
    work = tempfile.mkdtemp(prefix="schematron-crosscheck-")
    for test_set in catalog.findall(Q + "test-set"):
        if arguments.set and test_set.get("name") not in arguments.set:
            continue
        set_file = os.path.join(suite, test_set.get("file"))
        set_directory = os.path.join(suite, test_set.get("name"))
        for case in ElementTree.parse(set_file).getroot().findall(Q + "test-case"):
            name = case.get("name")
            if arguments.case and name not in arguments.case:
                continue
            if case.get("since", "2020") not in editions:
                continue
            bindings = [d.get("value") for d in case.findall(Q + "dependency")
                        if d.get("type") == "query-binding"]
            features = [d.get("value") for d in case.findall(Q + "dependency")
                        if d.get("type") == "feature" and d.get("value") != "svrl"]
            schema = os.path.join(set_directory, case.find(Q + "schema").get("file"))
            instance = os.path.join(set_directory, case.find(Q + "instance").get("file"))
            result = case.find(Q + "result")
            compare_lang = any(d.get(XML_LANG) for d in result.iter(Q + "diagnostic"))
            # The compiled stylesheet sits beside the schema, so that relative
            # URIs in its queries resolve as they would against the schema.
            compiled = os.path.join(os.path.dirname(schema), ".crosscheck-" + name + ".xsl")
            svrl = os.path.join(work, name + ".svrl")
            command = [arguments.sxml, "transform"]
            phase = case.findtext(Q + "phase")
            if phase is not None:
                command.append("--param=phase=" + phase)
            code, _, error = run(command + [arguments.schxslt, schema], compiled)
            stage = "compile"
            if code == 0:
                code, _, error = run([arguments.sxml, "transform", compiled, instance], svrl)
                stage = "validate"
            failed = code != 0
            if os.path.exists(compiled):
                os.remove(compiled)
            outcome = ("failed in %s: %s" % (stage, normalize(error.decode(errors="replace"))[:200])
                       if failed else "ran")
            verdict, detail = "agree", ""
            unsupported = [b for b in bindings if b not in SCHXSLT_BINDINGS]
            if unsupported or features:
                verdict = "not applicable"
                detail = "needs " + ", ".join(unsupported + features)
            elif result.find(Q + "implementation-defined") is not None:
                verdict = "implementation-defined"
                detail = outcome
                if not failed:
                    found, flags = actual_from_svrl(svrl, set_directory, instance)
                    detail = "; ".join("%s %s %s" % (f[0], f[1], f[3]) for f in found) or "no findings"
            elif result.find(Q + "findings") is None:
                if not failed:
                    verdict, detail = "disagree", "expected an error, SchXslt ran"
            elif failed:
                verdict, detail = "disagree", "expected findings, SchXslt " + outcome
            else:
                found, flags = actual_from_svrl(svrl, set_directory, instance)
                missing, extra = compare_findings(expected_findings(result, compare_lang), found,
                                                  compare_lang)
                problems = []
                if missing:
                    problems.append("missing " + repr(missing))
                if extra:
                    problems.append("unexpected " + repr(extra))
                expected_flags = result.findtext(Q + "flags")
                if expected_flags is not None and set(expected_flags.split()) != flags:
                    problems.append("flags %s, expected %s" % (sorted(flags), expected_flags.split()))
                svrl_tests = [e.text for e in result.findall(Q + "assert-svrl")]
                failing = svrl_assertions_hold(arguments.sxml, svrl, svrl_tests) if svrl_tests else []
                if failing:
                    problems.append("SVRL assertions false: " + repr(failing))
                if problems:
                    verdict, detail = "disagree", "; ".join(problems)
            counts[verdict] += 1
            line = "%-16s %-22s %s %s" % (verdict, test_set.get("name"), name,
                                          ("-- " + detail) if detail else "")
            lines.append(line)
            print(line, flush=True)
    summary = ", ".join("%s %d" % item for item in sorted(counts.items()))
    print(summary)
    if arguments.report:
        with open(arguments.report, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n" + summary + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
