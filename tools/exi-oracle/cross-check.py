#!/usr/bin/env python3
"""Has the oracle decode the streams sxml encodes (tests/exi/README.md).

    tools/exi-oracle/cross-check.py EXI_GOLDEN SET...

EXI_GOLDEN is the built driver (build/<preset>/tests/exi_golden). For each
case of the named sets, sxml encodes the input (exi_golden --encoded-dir),
EXIficient decodes that stream with the case's options, and its decoding
must be byte for byte the decoding of EXIficient's own stream (the golden
.xml). This checks what byte comparison cannot: the compression sets,
whose DEFLATE output differs between implementations. Needs Java (the
oracle), like make-golden.py.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    driver, sets = sys.argv[1], set(sys.argv[2:])
    rows = [line.rstrip("\n").split("\t") for line in open(os.path.join(ROOT, "tests", "exi", "manifest.tsv"))
            if not line.startswith("#")]
    rows = [row for row in rows if row[0] in sets and row[4] == "ok"]
    with tempfile.TemporaryDirectory() as scratch:
        encoded = os.path.join(scratch, "encoded")
        os.makedirs(encoded)
        for name in sets:
            subprocess.run([driver, ROOT, f"--filter={name}/", f"--encoded-dir={encoded}"], capture_output=True)
        manifest = os.path.join(scratch, "manifest.tsv")
        with open(manifest, "w") as handle:
            for row in rows:
                path = os.path.join(encoded, f"{row[0]}__{row[1]}.exi")
                if os.path.exists(path):
                    handle.write(f"{row[0]}__{row[1]}\t{path}\t{row[3]}\n")
        decoded = os.path.join(scratch, "decoded")
        subprocess.run([os.path.join(ROOT, "tools", "exi-oracle", "oracle.sh"), "batch", manifest, decoded],
                       check=True, capture_output=True)
        failures = 0
        checked = 0
        expected = {f"{row[0]}__{row[1]}": row[5] for row in rows}
        missing = len(rows)
        for line in open(os.path.join(decoded, "results.txt")):
            key, status = line.rstrip("\n").split("\t")[:2]
            missing -= 1
            checked += 1
            if status != "ok":
                failures += 1
                print(f"REFUSED {key}: {line.strip()}")
                continue
            with open(os.path.join(decoded, key + ".xml"), "rb") as handle:
                ours = handle.read()
            with open(os.path.join(ROOT, expected[key]), "rb") as handle:
                theirs = handle.read()
            if ours != theirs:
                failures += 1
                print(f"DIFFERS {key}")
        print(f"cross-check: {checked} streams sxml wrote, {failures} failures, {missing} not written")
        sys.exit(1 if failures or missing else 0)


if __name__ == "__main__":
    main()
