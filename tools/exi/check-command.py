#!/usr/bin/env python3
"""The acceptance check of packet 8.14 (docs/tasks/phase-8.md; D333): the
commands sxml encode and sxml decode, and --input-format=exi, against the
golden streams of tests/exi (which EXIficient wrote).

    tools/exi/check-command.py SXML [--sample=N]

For a sample of each schema-less set:

- `sxml encode OPTIONS --output=FILE INPUT` writes the golden stream byte
  for byte (compression: `sxml decode` of it gives the golden decoding);
- `sxml encode OPTIONS INPUT` with standard output a pipe writes the same
  bytes there;
- `sxml decode OPTIONS GOLDEN` writes XML text that is deep-equal to
  EXIficient's decoding;
- `sxml xpath --input-format=exi --context-item=GOLDEN EXPRESSION` (default
  options, bit-packed set) gives the result the XML input gives.

And: a stream that is not EXI is refused with exit status 1 and a message
on standard error; an unknown option value is a usage error (exit 2).

OPTIONS are the flags of the case's option specification
(tests/exi/README.md, "Options"): --alignment=..., --preserve=...,
--fragment, --block-size=N, --value-max-length=N,
--value-partition-capacity=N.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SETS = ["bit-packed", "bit-packed-full", "byte-alignment-full", "pre-compression", "pre-compression-full",
        "pre-compression-blocks", "compression", "compression-full", "compression-blocks", "string-limits",
        "string-limits-zero", "fragment"]


def flags(specification):
    result = []
    options = {}
    for part in specification.split(";"):
        if part:
            key, _, value = part.partition("=")
            options[key] = value
            if key in ("alignment", "preserve", "block-size", "value-max-length", "value-partition-capacity"):
                result.append(f"--{key}={value}")
            elif key == "fragment":
                result.append("--fragment")
    return result, options


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sxml")
    parser.add_argument("--sample", type=int, default=6)
    arguments = parser.parse_args()
    sxml = os.path.abspath(arguments.sxml)
    failures = 0
    count = 0

    def record(name, passed, detail=""):
        nonlocal failures, count
        count += 1
        if not passed:
            failures += 1
            print(f"FAIL {name}: {str(detail).strip()[:300]}")

    def run(*command, **keywords):
        return subprocess.run([sxml, *command], capture_output=True, cwd=ROOT, **keywords)

    def deep_equal(first, second, options):
        fidelity = options.get("preserve", "").split(",")
        comparison = ("{'comments': %s, 'processing-instructions': %s, 'namespace-prefixes': %s}" %
                      tuple("true()" if item in fidelity else "false()" for item in ("comments", "pis", "prefixes")))
        result = run("xquery", "--method=text", f"--expression=deep-equal(doc('{first}'), doc('{second}'), {comparison})")
        return result.stdout.decode().strip() == "true", result.stderr.decode()

    rows = [line.rstrip("\n").split("\t") for line in open(os.path.join(ROOT, "tests", "exi", "manifest.tsv"))
            if not line.startswith("#")]
    with tempfile.TemporaryDirectory() as scratch:
        for name in SETS:
            for row in [row for row in rows if row[0] == name and row[4] == "ok"][:arguments.sample]:
                case_flags, options = flags(row[3])
                golden = f"tests/exi/golden/{row[0]}/{row[1]}.exi"
                with open(os.path.join(ROOT, golden), "rb") as handle:
                    expected = handle.read()
                output = os.path.join(scratch, f"{row[0]}-{row[1]}.exi")
                result = run("encode", *case_flags, f"--output={output}", row[2])
                written = open(output, "rb").read() if os.path.exists(output) else b""
                if options.get("alignment") == "compression":
                    decoded = os.path.join(scratch, f"{row[0]}-{row[1]}.xml")
                    decode = run("decode", *case_flags, f"--output={decoded}", output)
                    equal, detail = deep_equal(decoded, row[5], options) if decode.returncode == 0 else (False, decode.stderr)
                    record(f"{name}/{row[1]}/encode", result.returncode == 0 and equal, result.stderr.decode() or detail)
                else:
                    record(f"{name}/{row[1]}/encode", result.returncode == 0 and written == expected,
                           result.stderr.decode() or f"{len(written)} bytes, expected {len(expected)}")
                    piped = run("encode", *case_flags, row[2])
                    record(f"{name}/{row[1]}/encode-stdout", piped.returncode == 0 and piped.stdout == expected,
                           piped.stderr.decode() or "bytes differ")
                decoded = os.path.join(scratch, f"{row[0]}-{row[1]}.decoded.xml")
                result = run("decode", *case_flags, f"--output={decoded}", golden)
                equal, detail = deep_equal(decoded, row[5], options) if result.returncode == 0 else (False, result.stderr)
                record(f"{name}/{row[1]}/decode", equal, detail)
        for row in [row for row in rows if row[0] == "bit-packed" and row[4] == "ok"][:arguments.sample]:
            expression = "string-join((count(//*), count(//@*), string-length(string(/))), ' ')"
            from_exi = run("xpath", "--input-format=exi", f"--context-item=tests/exi/golden/bit-packed/{row[1]}.exi",
                           expression)
            from_xml = run("xpath", f"--context-item={row[2]}", expression)
            record(f"input-format/{row[1]}", from_exi.returncode == 0 and from_exi.stdout == from_xml.stdout,
                   from_exi.stderr.decode() or f"{from_exi.stdout!r} != {from_xml.stdout!r}")
        not_exi = os.path.join(scratch, "not.exi")
        with open(not_exi, "wb") as handle:
            handle.write(b"<order/>")
        result = run("decode", not_exi)
        record("decode-refuses-text", result.returncode == 1 and len(result.stderr) > 0, result.stderr.decode())
        result = run("encode", "--alignment=sideways", "--output=" + os.path.join(scratch, "x.exi"),
                     "tests/exi/handmade/empty.xml")
        record("encode-usage-error", result.returncode == 2, result.stderr.decode())
    print(f"check-command: {count} checks, {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
