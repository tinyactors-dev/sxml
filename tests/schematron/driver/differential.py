#!/usr/bin/env python3
"""The differential test of packet 7.28 (gate 3): sxml's Schematron against
SchXslt 1.10.1 running on sxml, over real rule sets and their instances.

Usage:
  differential.py --sxml PATH-TO-sxml --driver PATH-TO-sxml-schematron-test
                  --schxslt schxslt-1.10.1/2.0/pipeline-for-svrl.xsl
                  --schema RULES.sch --work DIRECTORY [--phase PHASE]
                  INSTANCE...

SchXslt compiles RULES.sch into an XSLT stylesheet (`sxml transform
pipeline-for-svrl.xsl RULES.sch`, kept in DIRECTORY), which validates each
INSTANCE and writes SVRL. `sxml-schematron-test report --fired` gives
sxml's findings and fired rules for the same instances. For each instance
the script compares

- the set of (assertion id, kind, subject path) of the findings with the
  set of (id, kind, location) of SVRL's failed-assert and
  successful-report elements;
- the multiset of fired rules' contexts with SVRL's fired-rule contexts,
  whitespace normalized (SchXslt writes a context as written, sxml with
  its leading and trailing whitespace stripped, 5.3); SchXslt writes no
  node for a fired rule, so the node is not compared.

It prints one line per instance that differs and a summary; the exit
status is 1 when any differs. Differences are explained in the decision
record (D323), never reclassified (D178).
"""
import argparse
import collections
import hashlib
import os
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

SVRL = "{http://purl.oclc.org/dsdl/svrl}"


def compile_with_schxslt(arguments):
    key = hashlib.sha256((os.path.abspath(arguments.schema) + (arguments.phase or "")).encode())
    compiled = os.path.join(arguments.work, "schxslt-" + key.hexdigest()[:16] + ".xsl")
    if not os.path.exists(compiled):
        command = [arguments.sxml, "transform"]
        if arguments.phase:
            command.append("--param=phase=" + arguments.phase)
        command += [arguments.schxslt, arguments.schema]
        completed = subprocess.run(command, capture_output=True)
        if completed.returncode != 0:
            sys.exit("SchXslt failed: " + completed.stderr.decode(errors="replace")[:500])
        with open(compiled, "wb") as handle:
            handle.write(completed.stdout)
    return compiled


def schxslt_results(arguments, compiled, instance):
    completed = subprocess.run([arguments.sxml, "transform", compiled, instance],
                               capture_output=True)
    if completed.returncode != 0:
        return None, None, completed.stderr.decode(errors="replace")[:300]
    root = ElementTree.fromstring(completed.stdout)
    findings, fired = set(), collections.Counter()
    for element in root:
        if element.tag == SVRL + "fired-rule":
            fired[" ".join((element.get("context") or "").split())] += 1
        elif element.tag in (SVRL + "failed-assert", SVRL + "successful-report"):
            findings.add((element.get("id") or "", element.tag[len(SVRL):],
                          element.get("location") or ""))
    return findings, fired, None


def sxml_results(arguments, instances):
    command = [arguments.driver, "report", "--fired", "--schematron=" + arguments.schema]
    if arguments.phase:
        command.append("--phase=" + arguments.phase)
    completed = subprocess.run(command + instances, capture_output=True, text=True)
    results, current = {}, None
    for line in completed.stdout.splitlines():
        fields = line.split("\t")
        if fields[0] == "instance":
            current = results.setdefault(fields[1], [set(), collections.Counter(), None])
        elif fields[0] == "error":
            current[2] = line
        elif fields[0] == "fired-rule":
            current[1][" ".join(fields[2].split())] += 1
        else:
            current[0].add((fields[2], fields[0], fields[3]))
    if completed.returncode not in (0, 1) or not results:
        sys.exit("sxml failed: " + completed.stderr[:500])
    return results


def main():
    parser = argparse.ArgumentParser()
    for option in ("--sxml", "--driver", "--schxslt", "--schema", "--work"):
        parser.add_argument(option, required=True)
    parser.add_argument("--phase")
    parser.add_argument("instances", nargs="+")
    arguments = parser.parse_args()
    os.makedirs(arguments.work, exist_ok=True)
    compiled = compile_with_schxslt(arguments)
    ours = sxml_results(arguments, arguments.instances)
    counts = collections.Counter()
    for instance in arguments.instances:
        findings, fired, failure = schxslt_results(arguments, compiled, instance)
        own_findings, own_fired, own_failure = ours.get(instance, [set(), collections.Counter(),
                                                                    "missing"])
        problems = []
        if failure or own_failure:
            if not (failure and own_failure):
                problems.append("SchXslt: %s; sxml: %s" % (failure or "ran", own_failure or "ran"))
        else:
            if findings - own_findings:
                problems.append("only SchXslt %s" % sorted(findings - own_findings))
            if own_findings - findings:
                problems.append("only sxml %s" % sorted(own_findings - findings))
            if fired != own_fired:
                difference = (fired - own_fired) + (own_fired - fired)
                problems.append("fired rules differ %s" % sorted(difference.items())[:5])
        counts["instances"] += 1
        counts["findings"] += len(own_findings)
        counts["fired rules"] += sum(own_fired.values())
        if problems:
            counts["differ"] += 1
            print("DIFFER %s: %s" % (instance, "; ".join(problems)[:1000]))
        else:
            counts["agree"] += 1
    print("SUMMARY", ", ".join("%s %d" % item for item in sorted(counts.items())))
    return 1 if counts["differ"] else 0


if __name__ == "__main__":
    sys.exit(main())
